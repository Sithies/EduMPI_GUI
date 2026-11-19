#include "timeline_store.h"

#include <QSet>

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

quint64 pyramidBytes(int ranks, int levels)
{
    quint64 bins = 0;
    for (int k = 0; k < levels; ++k)
        bins += quint64(1024) << (2 * k);
    return quint64(qMax(0, ranks)) * bins * kCategoryCount * sizeof(quint16);
}

quint64 estimateTimelineBytes(quint64 eventCount, bool hasMetrics, int ranks, int pyramidLevels)
{
    const quint64 e = hasMetrics ? quint64(double(eventCount) * 0.6) : eventCount;
    return e / 2 * sizeof(Activity) + e / 4 * (sizeof(Message) + 8) + e / 8 * sizeof(UserSpan)
         + pyramidBytes(ranks, pyramidLevels);
}

} // namespace trace
