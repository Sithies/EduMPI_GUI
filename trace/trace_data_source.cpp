#include "trace_data_source.h"

#include <QDate>
#include <QDebug>
#include <QMutexLocker>
#include <QThread>
#include <QTime>

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

#include "memory_budget.h"
#include "timeline_builder.h"

using namespace trace;

namespace {

using Matrix = QVector<QVector<long>>;

// Decodes a coll_partnerranks bitmask exactly like Database_Thread::detailed_p2p_Query.
QVector<int> decodePartners(const QByteArray &bitmask)
{
    QVector<int> numbers;
    int bitIndex = 0;
    for (char byte : bitmask) {
        for (int i = 0; i < 8; i++) {
            if (byte & (1 << i)) {
                const int value = bitIndex + i;
                if (value <= 400)
                    numbers.append(value);
            }
        }
        bitIndex += 8;
    }
    return numbers;
}

// Grouping key of detail rows over several seconds (the DB's GROUP BY function,
// communicationtype, processrank, partnerrank, coll_algorithm, coll_partnerranks).
struct DetailGroupKey {
    QString commType;
    QString function;
    int rank;
    int partner;
    QByteArray collPartners;
    bool operator<(const DetailGroupKey &o) const
    {
        return std::tie(commType, function, rank, partner, collPartners)
            < std::tie(o.commType, o.function, o.rank, o.partner, o.collPartners);
    }
};

// Sink that decides on the load mode once the archive's metadata is known (begin()): it
// buffers the addRank calls, then creates the TimelineBuilder (in front of agg) for the
// whole run, keeping activities in full mode only.
class BudgetedTimelineSink : public TraceSink
{
public:
    BudgetedTimelineSink(TraceAggregator &agg, quint64 budget) : m_agg(agg), m_budget(budget) {}

    void addRank(int rank, const QString &node) override { m_ranks.append({rank, node}); }
    void begin(const TraceMeta &meta) override
    {
        m_plan = planTimelineLoad(meta, int(m_ranks.size()), m_budget);
        m_builder.emplace(m_agg, 0, std::numeric_limits<qint64>::max(), m_plan.full, m_plan.pyramidLevels);
        for (const auto &r : std::as_const(m_ranks))
            m_builder->addRank(r.first, r.second);
        m_builder->begin(meta);
    }
    void addCall(const MpiCall &call) override { sink().addCall(call); }
    void addSend(const P2PSend &send) override { sink().addSend(send); }
    void addRecv(const P2PRecv &recv) override { sink().addRecv(recv); }
    void addCollective(const Collective &coll) override { sink().addCollective(coll); }
    void addUserRegion(const UserRegion &region) override { sink().addUserRegion(region); }

    int rankCount() const { return int(m_ranks.size()); }
    // Call after agg.finish(); null if begin() never ran.
    std::shared_ptr<TimelineStore> finish() { return m_builder ? m_builder->finish() : nullptr; }

private:
    TraceSink &sink()
    {
        if (!m_builder) // readOtf2 always calls begin() first; be defensive anyway
            begin(TraceMeta{});
        return *m_builder;
    }

    TraceAggregator &m_agg;
    quint64 m_budget;
    QVector<QPair<int, QString>> m_ranks;
    TimelineLoadPlan m_plan;
    std::optional<TimelineBuilder> m_builder;
};

qint64 floorDiv(qint64 a, qint64 b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

constexpr qint64 kNsPerSecond = 1'000'000'000;

} // namespace

Trace_Data_Source::Trace_Data_Source(const QString &tracePathOrUrl, QObject *parent)
    : QObject(parent), m_input(tracePathOrUrl)
{
    qRegisterMetaType<trace::TimelineSnapshot>();
}

QDateTime Trace_Data_Source::startTimestamp() const
{
    if (m_start.isValid())
        return m_start;
    if (m_meta.realtimeStart.isValid())
        return m_meta.realtimeStart.toLocalTime();
    // Bottom_Bar treats a start time of 0 as "unset"; use a non-zero time of day.
    return QDateTime(QDate::currentDate(), QTime(0, 0, 1));
}

void Trace_Data_Source::connectToDB()
{
    emit connectedToDB(true);
}

void Trace_Data_Source::threadbuildClusterComponents()
{
    QString error;
    const QString anchor = resolveTracePath(m_input, &error);
    if (anchor.isEmpty()) {
        emit loadFailed(error.isEmpty() ? QStringLiteral("Cannot open trace: %1").arg(m_input) : error);
        return;
    }

    TraceAggregator agg;
    TraceMeta meta;
    const quint64 budget = memoryBudgetBytes();
    BudgetedTimelineSink sink(agg, budget);
    if (!readOtf2(anchor, sink, &meta, &error, &m_cancel)) {
        emit loadFailed(error.isEmpty() ? QStringLiteral("Cannot read trace: %1").arg(anchor) : error);
        return;
    }

    m_meta = meta;
    m_summary = agg.finish(meta.traceLengthNs);
    m_overview = sink.finish();
    m_detailStore.reset();
    m_anchor = anchor;
    m_budget = budget;
    m_rankCount = sink.rankCount();
    m_start = QDateTime();
    m_start = startTimestamp();

    // Size the matrices defensively: every rank / partner referenced must be indexable.
    int n = m_summary.rankCount;
    for (const auto &rows : std::as_const(m_summary.rows))
        for (const BucketRow &r : rows)
            n = std::max(n, r.rank + 1);
    for (const auto &details : std::as_const(m_summary.details)) {
        for (const DetailRow &d : details) {
            n = std::max({n, d.rank + 1, d.partner + 1});
            for (int p : decodePartners(d.collPartners))
                n = std::max(n, p + 1);
        }
    }
    m_matrixSize = n;

    m_loaded = true;
    m_started = false;
    m_current = -1;
    m_windowed.store(m_overview && !m_overview->complete);

    emit traceInfoReady(m_meta.name, m_summary.rankCount, double(m_meta.traceLengthNs) / 1e9,
                        m_summary.unmatchedMessages);
    m_maxDetailWindowNs = maxDetailWindowNs(m_meta, m_rankCount, m_budget);
    TimelineSnapshot overview{m_overview};
    overview.maxDetailWindowNs = m_maxDetailWindowNs;
    emit timelineReady(overview);
    emit clusterComponentsReady(m_summary.nodes);
}

void Trace_Data_Source::updateData(const int &time_display)
{
    if (!m_loaded)
        return;

    const int last = int(std::min(m_summary.rows.size(), m_summary.details.size())) - 1;
    if (!m_started) {
        m_started = true;
        m_current = 0;
        emit setTimestamp(m_start, true);
    } else {
        m_current = std::min(m_current + 1, std::max(last, 0));
    }

    if (time_display == 0)
        emitRows(m_current, m_current);
    else if (time_display == 1)
        emitRows(0, m_current);
    else
        emitRows(1, 0); // unknown mode: like the DB's empty query, no rows
    emitDetails(m_current, m_current);
}

void Trace_Data_Source::showDataFromTimePeriod(const QDateTime timestampA, const QDateTime timestampB)
{
    if (!m_loaded)
        return;
    // Seconds relative to the trace start; emitRows / emitDetails intersect with the trace.
    auto second = [this](const QDateTime &t) {
        return int(std::clamp<qint64>(m_start.secsTo(t), -1, std::numeric_limits<int>::max()));
    };
    const int from = second(timestampA);
    const int to = second(timestampB);
    emitRows(from, to);
    emitDetails(from, to);
}

void Trace_Data_Source::emitRows(int from, int to)
{
    from = std::max(from, 0);
    to = std::min(to, int(m_summary.rows.size()) - 1);
    QVector<const BucketRow *> rows;
    for (int sec = from; sec <= to; ++sec)
        for (const BucketRow &r : m_summary.rows.at(sec))
            rows.append(&r);
    emitBucketRows(rows);
}

void Trace_Data_Source::emitBucketRows(const QVector<const BucketRow *> &rows)
{
    // (rank, isCollective) -> summed row; p2p before collective per rank, ranks ascending.
    QMap<QPair<int, bool>, DataColumn> sums;
    for (const BucketRow *rp : rows) {
        const BucketRow &r = *rp;
        const QPair<int, bool> key(r.rank, r.commType == QLatin1String("collective"));
        auto it = sums.find(key);
        if (it == sums.end()) {
            DataColumn dc;
            dc.proc_name = r.node;
            dc.comm_type = r.commType;
            dc.proc_rank = r.rank;
            dc.recv_datasize = 0;
            dc.send_datasize = 0;
            dc.late_sender = 0.0f;
            dc.late_receiver = 0.0f;
            dc.time_diff = 0.0f;
            it = sums.insert(key, dc);
        }
        it->recv_datasize += long(r.recvBytes);
        it->send_datasize += long(r.sendBytes);
        it->late_sender += float(r.lateSender);
        it->late_receiver += float(r.lateReceiver);
        it->time_diff += float(r.timeDiff);
    }
    emit updateDataReady(sums.values());
}

void Trace_Data_Source::emitDetails(int from, int to)
{
    from = std::max(from, 0);
    to = std::min(to, int(m_summary.details.size()) - 1);
    QVector<const DetailRow *> details;
    for (int sec = from; sec <= to; ++sec)
        for (const DetailRow &d : m_summary.details.at(sec))
            details.append(&d);
    emitDetailRows(details);
}

void Trace_Data_Source::emitDetailRows(const QVector<const DetailRow *> &details)
{
    QMap<DetailGroupKey, QPair<qint64, qint64>> groups; // -> (send, recv)
    for (const DetailRow *d : details) {
        auto &sum = groups[{d->commType, d->function, d->rank, d->partner, d->collPartners}];
        sum.first += d->sendBytes;
        sum.second += d->recvBytes;
    }

    const int n = m_matrixSize;
    Matrix p2pSend(n, QVector<long>(n, 0));
    Matrix p2pRecv(n, QVector<long>(n, 0));
    Matrix collSend(n, QVector<long>(n, 0));
    Matrix collRecv(n, QVector<long>(n, 0));
    Matrix totalSend(n, QVector<long>(n, 0));
    auto inRange = [n](int i) { return i >= 0 && i < n; };

    QList<QVariantList> p2pList;
    QList<QVariantList> collList;
    QString s;

    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const DetailGroupKey &k = it.key();
        const long sendSize = long(it.value().first);
        const long recvSize = long(it.value().second);
        const int procRank = k.rank;
        const int partRank = k.partner;

        if (k.commType == QLatin1String("p2p")) {
            p2pList.append(QVariantList{k.function, procRank, partRank});
            // The DB skips MPI_Wait rows here: they carry partner -1 (bytes are on the
            // MPI_Irecv rows). Skip by that property, not by name.
            if (partRank < 0)
                continue;
            const QString fun = k.function + QStringLiteral(", ");
            if (!s.contains(fun))
                s.append(fun);
            if (!inRange(procRank) || !inRange(partRank))
                continue;
            if (sendSize > 0) {
                totalSend[procRank][partRank] += sendSize;
                p2pSend[procRank][partRank] += sendSize;
            }
            if (recvSize > 0)
                p2pRecv[procRank][partRank] += recvSize;
        } else if (k.commType == QLatin1String("collective")) {
            const QString algorithm; // not recorded in traces
            collList.append(QVariantList{k.function, procRank, algorithm, k.collPartners});
            const QString fun = k.function + QStringLiteral(" (") + algorithm + QStringLiteral("), ");
            if (!s.contains(fun))
                s.append(fun);

            if (!k.collPartners.isEmpty()) {
                const QVector<int> numbers = decodePartners(k.collPartners);
                if (numbers.isEmpty() || !inRange(procRank))
                    continue;
                const long send = sendSize > 0 ? sendSize / long(numbers.size()) : 0;
                for (int partner : numbers) {
                    if (!inRange(partner))
                        continue;
                    totalSend[procRank][partner] += send;
                    collSend[procRank][partner] += send;
                    collRecv[partner][procRank] += send;
                }
            }
        }
    }

    emit setFunctionsString(s);
    emit updateDetailedP2P(p2pList);
    emit updateDetailedColl(collList);

    emit setCommMatrixP2PSend(p2pSend);
    emit setCommMatrixP2PRecv(p2pRecv);
    emit setCommMatrixCollSend(collSend);
    emit setCommMatrixCollRecv(collRecv);

    emit setCommMatrixTotalSend(totalSend);
}

void Trace_Data_Source::clearDatabase()
{
    emit dbCleared();
}

void Trace_Data_Source::selectEndTimestamp()
{
    // The trace has a fixed end; nothing to wait for.
}

void Trace_Data_Source::set_end_timestamp_db(QDateTime)
{
    // Nothing to persist for a trace.
}

void Trace_Data_Source::reset_actual_timestamp()
{
    // Next updateData() shows second 0 again (without re-emitting the start timestamp,
    // like Database_Thread, which rewinds its current time to just before the start).
    if (m_started)
        m_current = -1;
}

void Trace_Data_Source::set_thread_running(bool running)
{
    m_cancel.store(!running);
    if (!running) {
        QMutexLocker lock(&m_detailMutex);
        m_detailCancel.store(true);
    }
}

void Trace_Data_Source::requestDetail(qint64 fromNs, qint64 toNs)
{
    if (!m_windowed.load())
        return;
    {
        QMutexLocker lock(&m_detailMutex);
        m_pendingFrom = std::min(fromNs, toNs);
        m_pendingTo = std::max(fromNs, toNs);
        ++m_detailGen;
        m_detailCancel.store(true); // a read for an older request stops early
    }
    if (QThread::currentThread() == thread())
        processDetailRequest();
    else
        QMetaObject::invokeMethod(this, &Trace_Data_Source::processDetailRequest, Qt::QueuedConnection);
}

void Trace_Data_Source::processDetailRequest()
{
    qint64 from = 0;
    qint64 to = 0;
    quint64 gen = 0;
    {
        QMutexLocker lock(&m_detailMutex);
        gen = m_detailGen.load();
        if (gen == m_detailDone)
            return; // already handled by an earlier invocation
        m_detailDone = gen;
        from = m_pendingFrom;
        to = m_pendingTo;
        m_detailCancel.store(m_cancel.load());
    }
    if (m_detailCancel.load() || !m_loaded) {
        // Stopped (set_thread_running(false)): answer with a null store so that a waiting
        // model leaves its loading state. A newer request (gen moved on) answers itself.
        if (gen == m_detailGen.load())
            emit timelineReady(TimelineSnapshot{nullptr, from, to});
        return;
    }

    // [from, to] widened by 50 % on each side; margins (then the window) shrunk to the cap.
    const qint64 len = std::max<qint64>(m_meta.traceLengthNs, 0);
    const qint64 maxLen = m_maxDetailWindowNs;
    const qint64 span = to - from;
    qint64 margin = span / 2;
    if (span + 2 * margin > maxLen)
        margin = std::max<qint64>(0, (maxLen - span) / 2);
    qint64 wFrom = from - margin;
    qint64 wTo = to + margin;
    if (wTo - wFrom > maxLen) {
        wFrom = from + span / 2 - maxLen / 2;
        wTo = wFrom + maxLen;
    }
    wFrom = std::clamp<qint64>(wFrom, 0, len);
    wTo = std::clamp<qint64>(wTo, wFrom, len);

    TraceAggregator agg;
    TimelineBuilder builder(agg, wFrom, wTo, true, 0);
    TraceMeta meta;
    QString error;
    const bool ok = readOtf2(m_anchor, builder, &meta, &error, &m_detailCancel);
    if (gen != m_detailGen.load())
        return; // superseded: the newer request is answered instead
    if (!ok || m_cancel.load()) { // failed, or the source was stopped during the read
        if (!m_cancel.load())
            qWarning() << "Trace detail load failed:" << error;
        emit timelineReady(TimelineSnapshot{nullptr, from, to});
        return;
    }
    agg.finish(meta.traceLengthNs);
    m_detailStore = builder.finish();
    emit timelineReady(TimelineSnapshot{m_detailStore, from, to});
}

void Trace_Data_Source::showTraceWindow(qint64 fromNs, qint64 toNs)
{
    if (!m_loaded)
        return;
    const qint64 len = std::max<qint64>(m_meta.traceLengthNs, 0);
    const qint64 from = std::clamp<qint64>(std::min(fromNs, toNs), 0, len);
    const qint64 to = std::clamp<qint64>(std::max(fromNs, toNs), 0, len);

    auto covers = [from, to](const std::shared_ptr<const TimelineStore> &s) {
        return s && (s->complete || (from >= s->fromNs && to <= s->toNs && s->pyramid.levels == 0));
    };
    std::shared_ptr<const TimelineStore> store;
    if (m_overview && m_overview->complete)
        store = m_overview;
    else if (covers(m_detailStore))
        store = m_detailStore;

    if (store) {
        const WindowAggregate w = store->aggregate(from, to);
        QVector<const BucketRow *> rows;
        for (const BucketRow &r : w.rows)
            rows.append(&r);
        QVector<const DetailRow *> details;
        for (const DetailRow &d : w.details)
            details.append(&d);
        emitBucketRows(rows);
        emitDetailRows(details);
        emit traceWindowShown(from, to, true);
        return;
    }

    // Not loaded in detail: whole seconds floor(from) .. ceil(to) - 1 (at least one second).
    const qint64 secFrom = floorDiv(from, kNsPerSecond);
    const qint64 secTo = std::max(secFrom, floorDiv(to + kNsPerSecond - 1, kNsPerSecond) - 1);
    const auto toInt = [](qint64 v) { return int(std::clamp<qint64>(v, -1, std::numeric_limits<int>::max())); };
    emitRows(toInt(secFrom), toInt(secTo));
    emitDetails(toInt(secFrom), toInt(secTo));
    emit traceWindowShown(secFrom * kNsPerSecond, (secTo + 1) * kNsPerSecond, false);
}
