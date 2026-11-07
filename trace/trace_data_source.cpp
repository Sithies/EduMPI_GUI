#include "trace_data_source.h"

#include <QDate>
#include <QTime>

#include <algorithm>
#include <limits>
#include <tuple>

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

} // namespace

Trace_Data_Source::Trace_Data_Source(const QString &tracePathOrUrl, QObject *parent)
    : QObject(parent), m_input(tracePathOrUrl)
{
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
    if (!readOtf2(anchor, agg, &meta, &error, &m_cancel)) {
        emit loadFailed(error.isEmpty() ? QStringLiteral("Cannot read trace: %1").arg(anchor) : error);
        return;
    }

    m_meta = meta;
    m_summary = agg.finish(meta.traceLengthNs);
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

    emit traceInfoReady(m_meta.name, m_summary.rankCount, double(m_meta.traceLengthNs) / 1e9,
                        m_summary.unmatchedMessages);
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

    // (rank, isCollective) -> summed row; p2p before collective per rank, ranks ascending.
    QMap<QPair<int, bool>, DataColumn> sums;
    for (int sec = from; sec <= to; ++sec) {
        for (const BucketRow &r : m_summary.rows.at(sec)) {
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
    }
    emit updateDataReady(sums.values());
}

void Trace_Data_Source::emitDetails(int from, int to)
{
    from = std::max(from, 0);
    to = std::min(to, int(m_summary.details.size()) - 1);

    QMap<DetailGroupKey, QPair<qint64, qint64>> groups; // -> (send, recv)
    for (int sec = from; sec <= to; ++sec) {
        for (const DetailRow &d : m_summary.details.at(sec)) {
            auto &sum = groups[{d.commType, d.function, d.rank, d.partner, d.collPartners}];
            sum.first += d.sendBytes;
            sum.second += d.recvBytes;
        }
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
            if (k.function == QLatin1String("MPI_Wait"))
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
}
