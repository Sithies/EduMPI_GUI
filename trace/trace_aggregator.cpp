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
    return std::tie(collective, rank, function, partner, collPartners)
         < std::tie(o.collective, o.rank, o.function, o.partner, o.collPartners);
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

void TraceAggregator::bookMatch(int sender, int receiver, const PendingSend &s, const PendingRecv &r)
{
    bucket(r.enterNs, receiver, false).lateSender += nsToSeconds(std::max<qint64>(0, s.enterNs - r.enterNs));
    if (s.enterNs < r.enterNs && r.enterNs < s.leaveNs)
        bucket(s.enterNs, sender, false).lateReceiver += nsToSeconds(r.enterNs - s.enterNs);
}

void TraceAggregator::addSend(const P2PSend &send)
{
    bucket(send.enterNs, send.rank, false).sendBytes += qint64(send.bytes);
    addDetail(send.enterNs, {false, send.function, send.rank, send.peer, {}}, qint64(send.bytes), 0);

    const MatchKey key{send.rank, send.peer, send.comm, send.tag};
    const PendingSend s{send.enterNs, send.leaveNs};
    auto it = m_pending.find(key);
    if (it != m_pending.end() && !it->recvs.isEmpty()) {
        bookMatch(send.rank, send.peer, s, it->recvs.dequeue());
        if (it->recvs.isEmpty() && it->sends.isEmpty())
            m_pending.erase(it);
        return;
    }
    m_pending[key].sends.enqueue(s);
}

void TraceAggregator::addRecv(const P2PRecv &recv)
{
    bucket(recv.enterNs, recv.rank, false).recvBytes += qint64(recv.bytes);
    addDetail(recv.enterNs, {false, recv.function, recv.rank, recv.peer, {}}, 0, qint64(recv.bytes));

    const MatchKey key{recv.peer, recv.rank, recv.comm, recv.tag};
    const PendingRecv r{recv.enterNs};
    auto it = m_pending.find(key);
    if (it != m_pending.end() && !it->sends.isEmpty()) {
        bookMatch(recv.peer, recv.rank, it->sends.dequeue(), r);
        if (it->recvs.isEmpty() && it->sends.isEmpty())
            m_pending.erase(it);
        return;
    }
    m_pending[key].recvs.enqueue(r);
}

void TraceAggregator::completeCollective(const PendingCollective &inst)
{
    qint64 maxEnter = 0;
    bool first = true;
    for (const Arrival &a : inst.arrived) {
        maxEnter = first ? a.enterNs : std::max(maxEnter, a.enterNs);
        first = false;
    }
    for (const Arrival &a : inst.arrived)
        bucket(a.enterNs, a.rank, true).lateSender += nsToSeconds(maxEnter - a.enterNs);
}

void TraceAggregator::addCollective(const Collective &coll)
{
    Bucket &b = bucket(coll.enterNs, coll.rank, true);
    b.sendBytes += qint64(coll.bytesSent);
    b.recvBytes += qint64(coll.bytesRecv);
    addDetail(coll.enterNs, {true, coll.function, coll.rank, -1, partnerMask(coll.members, coll.rank)},
              qint64(coll.bytesSent), qint64(coll.bytesRecv));

    const quint64 index = m_collSeq[qMakePair(coll.rank, coll.comm)]++;
    const auto instKey = qMakePair(coll.comm, index);
    PendingCollective &inst = m_pendingColl[instKey];
    inst.expected = std::max<int>(1, int(coll.members.size()));
    inst.arrived.append({coll.rank, coll.enterNs});
    if (inst.arrived.size() >= inst.expected) {
        completeCollective(inst);
        m_pendingColl.remove(instKey);
    }
}

TraceSummary TraceAggregator::finish(qint64 traceLengthNs)
{
    TraceSummary s;

    for (auto it = m_pendingColl.cbegin(); it != m_pendingColl.cend(); ++it)
        completeCollective(it.value());
    m_pendingColl.clear();
    m_collSeq.clear();

    for (const MatchQueues &q : std::as_const(m_pending))
        s.unmatchedMessages += int(q.sends.size() + q.recvs.size());
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

    for (int sec = 0; sec < m_buckets.size(); ++sec) {
        for (auto it = m_buckets[sec].cbegin(); it != m_buckets[sec].cend(); ++it) {
            const Bucket &b = it.value();
            const int rank = it.key().first;
            s.rows[sec].append({m_rankNodes.value(rank), rank,
                                it.key().second ? QStringLiteral("collective") : QStringLiteral("p2p"),
                                b.sendBytes, b.recvBytes, b.lateSender, b.lateReceiver, b.timeDiff});
        }
    }
    for (int sec = 0; sec < m_details.size(); ++sec) {
        for (auto it = m_details[sec].cbegin(); it != m_details[sec].cend(); ++it) {
            const DetailKey &k = it.key();
            s.details[sec].append({k.collective ? QStringLiteral("collective") : QStringLiteral("p2p"),
                                   k.function, k.rank, k.partner, k.collPartners,
                                   it.value().sendBytes, it.value().recvBytes});
        }
    }
    m_buckets.clear();
    m_details.clear();
    return s;
}

} // namespace trace
