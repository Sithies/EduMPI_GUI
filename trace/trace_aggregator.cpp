#include "trace_aggregator.h"

#include <QRegularExpression>

#include <algorithm>
#include <tuple>

namespace trace {

namespace {
constexpr qint64 kNsPerSecond = 1'000'000'000;

double nsToSeconds(qint64 ns)
{
    return double(ns) / double(kNsPerSecond);
}

QByteArray partnerMask(const QVector<int> &members, int self)
{
    int maxRank = -1;
    for (int m : members)
        maxRank = std::max(maxRank, m);
    QByteArray mask((maxRank + 8) / 8, '\0');
    for (int m : members) {
        if (m == self || m < 0)
            continue;
        mask[m / 8] = char(uchar(mask[m / 8]) | (1u << (m % 8)));
    }
    return mask;
}
} // namespace

CallKind classifyMpiFunction(const QString &name)
{
    static const QRegularExpression p2p(QStringLiteral(
        "^MPI_(\\w*send|\\w*recv|Sendrecv\\w*|Wait\\w*|Test\\w*|\\w*probe)$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression coll(QStringLiteral(
        "^MPI_I?(Barrier|Bcast|Reduce\\w*|Allreduce|Scan|Exscan|\\w*gather\\w*|\\w*scatter\\w*|Alltoall\\w*)$"),
        QRegularExpression::CaseInsensitiveOption);
    // Collective first: e.g. "MPI_Reduce_scatter" must not fall into a p2p pattern.
    if (coll.match(name).hasMatch())
        return CallKind::Collective;
    if (p2p.match(name).hasMatch())
        return CallKind::P2P;
    return CallKind::OtherMpi;
}

size_t qHash(const TraceAggregator::MatchKey &k, size_t seed)
{
    return qHashMulti(seed, k.sender, k.receiver, k.comm, k.tag);
}

bool TraceAggregator::DetailKey::operator<(const DetailKey &o) const
{
    return std::tie(collective, rank, function, partner, collPartners, comm)
         < std::tie(o.collective, o.rank, o.function, o.partner, o.collPartners, o.comm);
}

bool TraceAggregator::DetailKey::operator==(const DetailKey &o) const
{
    return collective == o.collective && rank == o.rank && partner == o.partner && comm == o.comm
        && function == o.function && collPartners == o.collPartners;
}

size_t qHash(const TraceAggregator::DetailKey &k, size_t seed)
{
    return qHashMulti(seed, k.collective, k.rank, k.partner, k.comm, k.function, k.collPartners);
}

int TraceAggregator::secondOf(qint64 ns)
{
    return ns <= 0 ? 0 : int(ns / kNsPerSecond);
}

TraceAggregator::Bucket &TraceAggregator::bucket(qint64 ns, int rank, bool collective)
{
    const int sec = secondOf(ns);
    if (m_buckets.size() <= sec)
        m_buckets.resize(sec + 1);
    return m_buckets[sec][BucketKey(rank, collective)];
}

void TraceAggregator::addDetail(qint64 ns, const DetailKey &key, qint64 sendBytes, qint64 recvBytes)
{
    const int sec = secondOf(ns);
    if (m_details.size() <= sec)
        m_details.resize(sec + 1);
    DetailSums &d = m_details[sec][key];
    d.sendBytes += sendBytes;
    d.recvBytes += recvBytes;
}

void TraceAggregator::addRank(int rank, const QString &node)
{
    m_rankNodes.insert(rank, node);
}

void TraceAggregator::addCall(const MpiCall &call)
{
    if (call.kind == CallKind::OtherMpi)
        return;
    bucket(call.enterNs, call.rank, call.kind == CallKind::Collective).timeDiff
        += nsToSeconds(call.leaveNs - call.enterNs);
}

void TraceAggregator::bookMatch(const P2PSend &s, const P2PRecv &r)
{
    const double lateSender = nsToSeconds(std::max<qint64>(0, s.enterNs - r.enterNs));
    const double lateReceiver = s.enterNs < r.enterNs && r.enterNs < s.leaveNs
        ? nsToSeconds(r.enterNs - s.enterNs)
        : 0.0;
    bucket(r.enterNs, r.rank, false).lateSender += lateSender;
    if (lateReceiver > 0.0)
        bucket(s.enterNs, s.rank, false).lateReceiver += lateReceiver;
    if (m_listener)
        m_listener->onMatched({s, r, lateSender, lateReceiver});
}

void TraceAggregator::addSend(const P2PSend &send)
{
    bucket(send.enterNs, send.rank, false).sendBytes += qint64(send.bytes);
    addDetail(send.enterNs, {false, send.function, send.rank, send.peer, {}, send.comm}, qint64(send.bytes), 0);

    const MatchKey key{send.rank, send.peer, send.comm, send.tag};
    auto it = m_pending.find(key);
    if (it != m_pending.end() && !it->recvs.isEmpty()) {
        bookMatch(send, it->recvs.dequeue());
        if (it->recvs.isEmpty() && it->sends.isEmpty())
            m_pending.erase(it);
        return;
    }
    m_pending[key].sends.enqueue(send);
}

void TraceAggregator::addRecv(const P2PRecv &recv)
{
    bucket(recv.enterNs, recv.rank, false).recvBytes += qint64(recv.bytes);
    addDetail(recv.enterNs, {false, recv.function, recv.rank, recv.peer, {}, recv.comm}, 0, qint64(recv.bytes));

    const MatchKey key{recv.peer, recv.rank, recv.comm, recv.tag};
    auto it = m_pending.find(key);
    if (it != m_pending.end() && !it->sends.isEmpty()) {
        bookMatch(it->sends.dequeue(), recv);
        if (it->recvs.isEmpty() && it->sends.isEmpty())
            m_pending.erase(it);
        return;
    }
    m_pending[key].recvs.enqueue(recv);
}

void TraceAggregator::completeCollective(const PendingCollective &inst)
{
    qint64 maxEnter = 0;
    bool first = true;
    for (const Collective &a : inst.arrived) {
        maxEnter = first ? a.enterNs : std::max(maxEnter, a.enterNs);
        first = false;
    }
    CompletedCollective done;
    if (m_listener) {
        done.members = inst.arrived;
        done.lateSender.reserve(inst.arrived.size());
    }
    for (const Collective &a : inst.arrived) {
        const double late = nsToSeconds(maxEnter - a.enterNs);
        bucket(a.enterNs, a.rank, true).lateSender += late;
        if (m_listener)
            done.lateSender.append(late);
    }
    if (m_listener)
        m_listener->onCollective(done);
}

QByteArray TraceAggregator::partnerMaskOf(const Collective &coll)
{
    CommMask &cm = m_commMasks[coll.comm];
    if (cm.members.constData() != coll.members.constData() && cm.members != coll.members) {
        cm.members = coll.members;
        cm.mask = partnerMask(coll.members, -1);
    }
    // = partnerMask(coll.members, coll.rank): the full mask without the rank's own bit.
    QByteArray mask = cm.mask;
    const int self = coll.rank;
    if (self >= 0 && self / 8 < mask.size())
        mask[self / 8] = char(uchar(mask[self / 8]) & ~(1u << (self % 8)));
    return mask;
}

void TraceAggregator::addCollective(const Collective &coll)
{
    Bucket &b = bucket(coll.enterNs, coll.rank, true);
    b.sendBytes += qint64(coll.bytesSent);
    b.recvBytes += qint64(coll.bytesRecv);
    addDetail(coll.enterNs, {true, coll.function, coll.rank, -1, partnerMaskOf(coll), coll.comm},
              qint64(coll.bytesSent), qint64(coll.bytesRecv));

    const quint64 index = m_collSeq[qMakePair(coll.rank, coll.comm)]++;
    const auto instKey = qMakePair(coll.comm, index);
    PendingCollective &inst = m_pendingColl[instKey];
    inst.expected = std::max<int>(1, int(coll.members.size()));
    inst.arrived.append(coll);
    if (inst.arrived.size() >= inst.expected) {
        completeCollective(inst);
        m_pendingColl.remove(instKey);
    }
}

struct MatchState {
    QHash<TraceAggregator::MatchKey, TraceAggregator::MatchQueues> pending;
    QHash<QPair<quint64, quint64>, TraceAggregator::PendingCollective> pendingColl;
    QHash<QPair<int, quint64>, quint64> collSeq;
};

std::shared_ptr<const MatchState> TraceAggregator::saveMatchState() const
{
    auto s = std::make_shared<MatchState>();
    s->pending = m_pending;
    s->pendingColl = m_pendingColl;
    s->collSeq = m_collSeq;
    return s;
}

void TraceAggregator::restoreMatchState(const std::shared_ptr<const MatchState> &state)
{
    if (!state)
        return;
    m_pending = state->pending;
    m_pendingColl = state->pendingColl;
    m_collSeq = state->collSeq;
}

bool TraceAggregator::waitingFor(qint64 fromNs, qint64 toNs) const
{
    const auto overlaps = [fromNs, toNs](qint64 enter, qint64 leave) { return leave >= fromNs && enter <= toNs; };
    for (const MatchQueues &q : m_pending) {
        for (const P2PSend &s : q.sends)
            if (overlaps(s.enterNs, s.leaveNs))
                return true;
        for (const P2PRecv &r : q.recvs)
            if (overlaps(r.enterNs, r.leaveNs))
                return true;
    }
    for (const PendingCollective &inst : m_pendingColl)
        for (const Collective &c : inst.arrived)
            if (overlaps(c.enterNs, c.leaveNs))
                return true;
    return false;
}

TraceSummary TraceAggregator::finish(qint64 traceLengthNs)
{
    TraceSummary s;

    for (auto it = m_pendingColl.cbegin(); it != m_pendingColl.cend(); ++it)
        completeCollective(it.value());
    m_pendingColl.clear();
    m_collSeq.clear();
    m_commMasks.clear();

    for (const MatchQueues &q : std::as_const(m_pending)) {
        s.unmatchedMessages += int(q.sends.size() + q.recvs.size());
        if (m_listener) {
            for (const P2PSend &send : q.sends)
                m_listener->onUnmatchedSend(send);
            for (const P2PRecv &recv : q.recvs)
                m_listener->onUnmatchedRecv(recv);
        }
    }
    m_pending.clear();

    for (auto it = m_rankNodes.cbegin(); it != m_rankNodes.cend(); ++it)
        s.nodes[it.value()].append(it.key()); // QMap iteration: ranks ascending
    s.rankCount = int(m_rankNodes.size());

    const qint64 len = std::max<qint64>(0, traceLengthNs);
    s.seconds = int((len + kNsPerSecond - 1) / kNsPerSecond);
    // Never drop activity booked past the declared length.
    s.seconds = std::max({s.seconds, int(m_buckets.size()), int(m_details.size())});
    s.rows.resize(s.seconds);
    s.details.resize(s.seconds);

    const QString p2p = QStringLiteral("p2p");
    const QString collective = QStringLiteral("collective");
    for (int sec = 0; sec < m_buckets.size(); ++sec) {
        const QHash<BucketKey, Bucket> &buckets = m_buckets[sec];
        QVector<BucketKey> keys = buckets.keys();
        std::sort(keys.begin(), keys.end());
        s.rows[sec].reserve(keys.size());
        for (const BucketKey &key : std::as_const(keys)) {
            const Bucket &b = buckets[key];
            s.rows[sec].append({m_rankNodes.value(key.first), key.first, key.second ? collective : p2p,
                                b.sendBytes, b.recvBytes, b.lateSender, b.lateReceiver, b.timeDiff});
        }
        m_buckets[sec] = {}; // released second by second: lower peak while the rows grow
    }
    for (int sec = 0; sec < m_details.size(); ++sec) {
        const QHash<DetailKey, DetailSums> &details = m_details[sec];
        QVector<const DetailKey *> keys;
        keys.reserve(details.size());
        for (auto it = details.cbegin(); it != details.cend(); ++it)
            keys.append(&it.key());
        std::sort(keys.begin(), keys.end(), [](const DetailKey *a, const DetailKey *b) { return *a < *b; });
        s.details[sec].reserve(keys.size());
        for (const DetailKey *k : std::as_const(keys)) {
            const DetailSums &d = details[*k];
            s.details[sec].append({k->collective ? collective : p2p, k->function, k->rank, k->partner,
                                   k->collPartners, d.sendBytes, d.recvBytes, k->comm});
        }
        keys = {};
        m_details[sec] = {};
    }
    m_buckets.clear();
    m_details.clear();
    return s;
}

} // namespace trace
