#include "timeline_store.h"

#include <QMap>
#include <QSet>

#include <algorithm>
#include <tuple>

namespace trace {

namespace {
template<typename T>
quint64 vectorBytes(const QVector<T> &v)
{
    return quint64(v.capacity()) * sizeof(T);
}

quint64 stringBytes(const QString &s)
{
    return quint64(s.capacity()) * sizeof(QChar);
}
} // namespace

Category categorize(const QString &function, CallKind kind)
{
    switch (kind) {
    case CallKind::P2P:
        return function.contains(QLatin1String("send"), Qt::CaseInsensitive)
                    && !function.contains(QLatin1String("recv"), Qt::CaseInsensitive)
            ? Category::Send
            : Category::Recv;
    case CallKind::Collective:
        return Category::Collective;
    case CallKind::OtherMpi:
        break;
    }
    return Category::OtherMpi;
}

quint64 TimelineStore::bytesUsed() const
{
    // Payload only: per-row and per-level bookkeeping (RankRow / container headers, node
    // names; O(ranks) bytes, a few KB even at 256 ranks) is left out.
    quint64 b = vectorBytes(messages) + vectorBytes(collectives);
    for (const RankRow &r : rows)
        b += vectorBytes(r.activities) + vectorBytes(r.links) + vectorBytes(r.messages) + vectorBytes(r.userSpans);
    for (const RankRow &r : rows) {
        for (const QVector<qint32> &d : r.spansByDepth)
            b += vectorBytes(d);
        for (const QVector<qint32> &d : r.spansByDepthCoarse)
            b += vectorBytes(d);
    }
    // Member lists are implicitly shared between entries of the same communicator.
    QSet<const int *> memberLists;
    for (const CollectiveEntry &c : collectives) {
        if (!memberLists.contains(c.members.constData())) {
            memberLists.insert(c.members.constData());
            b += vectorBytes(c.members);
        }
    }
    for (const QString &f : functions)
        b += stringBytes(f);
    for (const auto &level : pyramid.data)
        for (const auto &bins : level)
            b += vectorBytes(bins);
    return b;
}

int TimelineStore::rowIndexOfRank(int rank) const
{
    // Rows are sorted by rank and usually dense (row index == rank).
    if (rank >= 0 && rank < rows.size() && rows[rank].rank == rank)
        return rank;
    int lo = 0;
    int hi = int(rows.size());
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (rows[mid].rank < rank)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < rows.size() && rows[lo].rank == rank ? lo : -1;
}

namespace {
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

struct Sums {
    qint64 sendBytes = 0;
    qint64 recvBytes = 0;
    double lateSender = 0;
    double lateReceiver = 0;
    double timeDiff = 0;
};
struct DetailKey {
    bool collective;
    QString function;
    int rank;
    int partner;
    QByteArray collPartners;
    bool operator<(const DetailKey &o) const
    {
        return std::tie(collective, rank, function, partner, collPartners)
             < std::tie(o.collective, o.rank, o.function, o.partner, o.collPartners);
    }
};
} // namespace

WindowAggregate TimelineStore::aggregate(qint64 from, qint64 to) const
{
    WindowAggregate out;
    if (!complete && !(from >= fromNs && to <= toNs))
        return out;

    QMap<QPair<int, bool>, Sums> buckets; // (rank, collective), as in stage 1
    QMap<DetailKey, QPair<qint64, qint64>> details;

    for (const RankRow &r : rows) {
        auto first = std::lower_bound(r.activities.cbegin(), r.activities.cend(), from,
                                      [](const Activity &a, qint64 t) { return a.startNs < t; });
        for (auto it = first; it != r.activities.cend() && it->startNs < to; ++it) {
            const Activity &a = *it;
            if (a.category == Category::OtherMpi || a.category == Category::Compute)
                continue;
            const bool coll = a.category == Category::Collective;
            Sums &b = buckets[{r.rank, coll}];
            b.timeDiff += double(a.endNs - a.startNs) / 1e9;
            for (int i = 0; i < a.linkCount; ++i) {
                const qint32 link = r.links[a.firstLink + i];
                // A message to self inside one call (MPI_Sendrecv) is linked twice: handle once.
                if (!coll && std::find(r.links.cbegin() + a.firstLink, r.links.cbegin() + a.firstLink + i, link)
                        != r.links.cbegin() + a.firstLink + i)
                    continue;
                if (coll) {
                    const CollectiveEntry &c = collectives[link];
                    b.sendBytes += qint64(c.bytesSent);
                    b.recvBytes += qint64(c.bytesRecv);
                    b.lateSender += c.lateSender;
                    auto &d = details[{true, functions.value(a.function), r.rank, -1,
                                       partnerMask(c.members, r.rank)}];
                    d.first += qint64(c.bytesSent);
                    d.second += qint64(c.bytesRecv);
                    continue;
                }
                const Message &m = messages[link];
                if (m.sender == r.rank && m.sendNs >= 0 && m.sendNs == a.startNs) {
                    b.sendBytes += qint64(m.bytes);
                    b.lateReceiver += m.lateReceiver;
                    details[{false, functions.value(m.sendFunction), r.rank, m.destination, {}}].first
                        += qint64(m.bytes);
                }
                if (m.receiver == r.rank && m.recvNs == a.endNs) {
                    b.recvBytes += qint64(m.bytes);
                    b.lateSender += m.lateSender;
                    details[{false, functions.value(m.recvFunction), r.rank, m.sender, {}}].second
                        += qint64(m.bytes);
                }
            }
        }
    }

    for (auto it = buckets.cbegin(); it != buckets.cend(); ++it) {
        const int rank = it.key().first;
        const int row = rowIndexOfRank(rank);
        const Sums &b = it.value();
        out.rows.append({row >= 0 ? rows[row].node : QString(), rank,
                         it.key().second ? QStringLiteral("collective") : QStringLiteral("p2p"),
                         b.sendBytes, b.recvBytes, b.lateSender, b.lateReceiver, b.timeDiff});
    }
    for (auto it = details.cbegin(); it != details.cend(); ++it) {
        const DetailKey &k = it.key();
        out.details.append({k.collective ? QStringLiteral("collective") : QStringLiteral("p2p"), k.function,
                            k.rank, k.partner, k.collPartners, it.value().first, it.value().second});
    }
    return out;
}

quint64 pyramidBytes(int ranks, int levels)
{
    quint64 bins = 0;
    for (int k = 0; k < levels; ++k)
        bins += quint64(1024) << (2 * k);
    return quint64(qMax(0, ranks)) * bins * kCategoryCount * sizeof(quint16);
}

void buildSpanIndex(RankRow &row, qint64 traceLengthNs)
{
    const int depths = row.userSpans.isEmpty() ? 0 : row.maxDepth + 1;
    const qint64 minCoarse = coarseSpanNs(traceLengthNs);
    QVector<int> fine(depths, 0);
    QVector<int> coarse(depths, 0);
    for (const UserSpan &u : row.userSpans) {
        if (u.depth >= depths)
            continue;
        ++fine[u.depth];
        if (u.endNs - u.startNs >= minCoarse)
            ++coarse[u.depth];
    }
    row.spansByDepth = QVector<QVector<qint32>>(depths);
    row.spansByDepthCoarse = QVector<QVector<qint32>>(depths);
    for (int d = 0; d < depths; ++d) {
        row.spansByDepth[d].reserve(fine[d]);
        row.spansByDepthCoarse[d].reserve(coarse[d]);
    }
    for (qsizetype i = 0; i < row.userSpans.size(); ++i) {
        const UserSpan &u = row.userSpans[i];
        if (u.depth >= depths)
            continue;
        row.spansByDepth[u.depth].append(qint32(i));
        if (u.endNs - u.startNs >= minCoarse)
            row.spansByDepthCoarse[u.depth].append(qint32(i));
    }
}

quint64 estimateTimelineBytes(quint64 eventCount, bool hasMetrics, int ranks, int pyramidLevels)
{
    const quint64 e = hasMetrics ? quint64(double(eventCount) * 0.6) : eventCount;
    return e / 2 * sizeof(Activity) + e / 4 * (sizeof(Message) + 8) + e / 8 * (sizeof(UserSpan) + 2 * sizeof(qint32))
         + pyramidBytes(ranks, pyramidLevels);
}

} // namespace trace
