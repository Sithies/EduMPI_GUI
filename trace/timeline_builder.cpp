#include "timeline_builder.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>

namespace trace {

namespace {
constexpr int kMaxPyramidLevels = 8;
constexpr int kRecentWaits = 64;
constexpr int kMaxRank = 1 << 24;
constexpr qint64 kUnitsPerBin = 65535;

int catIndex(Category c)
{
    return int(c);
}

void addSaturating(quint16 &v, qint64 delta)
{
    v = quint16(std::clamp<qint64>(qint64(v) + delta, 0, 65535));
}
} // namespace

TimelineBuilder::TimelineBuilder(TraceAggregator &agg, qint64 fromNs, qint64 toNs, bool keepActivities,
                                 int pyramidLevels)
    : m_agg(agg)
    , m_store(std::make_shared<TimelineStore>())
    , m_from(fromNs)
    , m_to(toNs)
    , m_keep(keepActivities)
    , m_levels(std::clamp(pyramidLevels, 0, kMaxPyramidLevels))
{
    m_agg.setListener(this);
}

TimelineBuilder::~TimelineBuilder()
{
    m_agg.setListener(nullptr);
}

// ---------------------------------------------------------------------------------------
// Rows, functions, window

int TimelineBuilder::rowIndex(int rank)
{
    if (rank < 0 || rank >= kMaxRank)
        return -1;
    if (rank < m_rankToRow.size() && m_rankToRow[rank] >= 0)
        return m_rankToRow[rank];
    if (m_rankToRow.size() <= rank)
        m_rankToRow.resize(rank + 1, -1);
    RankRow r;
    r.rank = rank;
    m_store->rows.append(r);
    m_rowState.append(RowState{});
    const int idx = int(m_store->rows.size()) - 1;
    m_rankToRow[rank] = idx;
    Pyramid &p = m_store->pyramid;
    for (int k = 0; k < p.levels; ++k)
        p.data[k].append(QVector<quint16>(qsizetype(p.binsPerLevel[k]) * kCategoryCount, 0));
    return idx;
}

quint16 TimelineBuilder::functionIndex(const QString &name)
{
    const auto it = m_functionIndex.constFind(name);
    if (it != m_functionIndex.cend())
        return it.value();
    if (m_store->functions.size() >= 65535) {
        // Table full (never seen in practice): all further names share one last entry.
        if (m_store->functions.size() == 65535)
            m_store->functions.append(QStringLiteral("(other)"));
        return 65535;
    }
    const quint16 idx = quint16(m_store->functions.size());
    m_store->functions.append(name);
    m_functionIndex.insert(name, idx);
    return idx;
}

bool TimelineBuilder::overlapsWindow(qint64 start, qint64 end) const
{
    return end >= m_from && start <= m_to;
}

int TimelineBuilder::findActivity(int row, qint64 startNs, qint64 endNs) const
{
    const QVector<Activity> &acts = m_store->rows[row].activities;
    auto it = std::lower_bound(acts.cbegin(), acts.cend(), startNs,
                               [](const Activity &a, qint64 s) { return a.startNs < s; });
    if (it == acts.cend() || it->startNs != startNs)
        return -1;
    for (auto j = it; j != acts.cend() && j->startNs == startNs; ++j)
        if (j->endNs == endNs)
            return int(j - acts.cbegin());
    return int(it - acts.cbegin());
}

// ---------------------------------------------------------------------------------------
// Pyramid

// Adds (delta = +1) or removes (-1) the share of [start, end] to category cat on every level.
// Positions are rounded to 1/65535 of a bin before differencing, so adjacent intervals add
// up without accumulating rounding errors.
void TimelineBuilder::addToPyramid(int row, qint64 start, qint64 end, Category cat, bool subtract)
{
    if (!m_pyramidOn || row < 0 || end <= start)
        return;
    Pyramid &p = m_store->pyramid;
    const int c = catIndex(cat);
    for (int k = 0; k < p.levels; ++k) {
        const qint64 bins = p.binsPerLevel[k];
        const qint64 maxUnits = bins * kUnitsPerBin;
        const double scale = m_binsPerNs[k] * double(kUnitsPerBin);
        const qint64 u0 = std::clamp<qint64>(std::llround(double(start) * scale), 0, maxUnits);
        const qint64 u1 = std::clamp<qint64>(std::llround(double(end) * scale), 0, maxUnits);
        if (u1 <= u0)
            continue;
        quint16 *data = p.data[k][row].data();
        const qint64 b0 = u0 / kUnitsPerBin;
        const qint64 b1 = std::min(bins - 1, (u1 - 1) / kUnitsPerBin);
        for (qint64 b = b0; b <= b1; ++b) {
            const qint64 lo = std::max(u0, b * kUnitsPerBin);
            const qint64 hi = std::min(u1, (b + 1) * kUnitsPerBin);
            const qint64 v = hi - lo;
            if (v <= 0)
                continue;
            quint16 *bin = data + b * kCategoryCount;
            if (subtract) {
                addSaturating(bin[c], -v);
                addSaturating(bin[catIndex(Category::Wait)], v);
            } else {
                addSaturating(bin[c], v);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// Sink

void TimelineBuilder::addRank(int rank, const QString &node)
{
    m_agg.addRank(rank, node);
    const int r = rowIndex(rank);
    if (r >= 0)
        m_store->rows[r].node = node;
}

void TimelineBuilder::begin(const TraceMeta &meta)
{
    m_agg.begin(meta);

    // Rows ascending by rank (addRank comes in definition order).
    QVector<int> order(m_store->rows.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [this](int a, int b) { return m_store->rows[a].rank < m_store->rows[b].rank; });
    QVector<RankRow> rows;
    QVector<RowState> states;
    rows.reserve(order.size());
    states.reserve(order.size());
    for (int i : order) {
        rows.append(std::move(m_store->rows[i]));
        states.append(std::move(m_rowState[i]));
    }
    m_store->rows = std::move(rows);
    m_rowState = std::move(states);
    for (int i = 0; i < m_store->rows.size(); ++i)
        m_rankToRow[m_store->rows[i].rank] = i;

    m_maxNs = std::max(m_maxNs, meta.traceLengthNs);
    m_minOverviewSpanNs = meta.traceLengthNs > 0 ? meta.traceLengthNs / 4096 : 0;
    m_pyramidOn = m_levels > 0 && m_from <= 0 && m_to >= meta.traceLengthNs && meta.traceLengthNs > 0;
    if (!m_pyramidOn)
        return;
    Pyramid &p = m_store->pyramid;
    p.levels = m_levels;
    p.traceLengthNs = meta.traceLengthNs;
    p.binsPerLevel.clear();
    p.data.clear();
    m_binsPerNs.clear();
    for (int k = 0; k < m_levels; ++k) {
        const int bins = 1024 << (2 * k);
        p.binsPerLevel.append(bins);
        m_binsPerNs.append(double(bins) / double(meta.traceLengthNs));
        QVector<QVector<quint16>> level;
        level.reserve(m_store->rows.size());
        for (int r = 0; r < m_store->rows.size(); ++r)
            level.append(QVector<quint16>(qsizetype(bins) * kCategoryCount, 0));
        p.data.append(std::move(level));
    }
}

void TimelineBuilder::addCall(const MpiCall &call)
{
    m_agg.addCall(call);
    const qint64 start = call.enterNs;
    const qint64 end = std::max(call.leaveNs, call.enterNs);
    m_maxNs = std::max(m_maxNs, end);
    const int row = rowIndex(call.rank);
    if (row < 0)
        return;
    const Category cat = categorize(call.function, call.kind);
    addToPyramid(row, start, end, cat, false);
    if (!m_keep || !overlapsWindow(start, end))
        return;
    const Activity a{start, end, 0, 0, 0, functionIndex(call.function), cat};
    QVector<Activity> &acts = m_store->rows[row].activities;
    if (acts.isEmpty() || acts.last().startNs <= start) {
        acts.append(a);
    } else {
        // Nested MPI regions / several threads per rank: leave order != enter order (rare).
        auto it = std::upper_bound(acts.begin(), acts.end(), start,
                                   [](qint64 s, const Activity &x) { return s < x.startNs; });
        acts.insert(it, a);
    }
}

void TimelineBuilder::addSend(const P2PSend &send)
{
    m_maxNs = std::max(m_maxNs, send.leaveNs);
    m_agg.addSend(send);
}

void TimelineBuilder::addRecv(const P2PRecv &recv)
{
    m_maxNs = std::max(m_maxNs, recv.leaveNs);
    m_agg.addRecv(recv);
}

void TimelineBuilder::addCollective(const Collective &coll)
{
    m_maxNs = std::max(m_maxNs, coll.leaveNs);
    m_agg.addCollective(coll);
}

void TimelineBuilder::addUserRegion(const UserRegion &region)
{
    m_agg.addUserRegion(region);
    const qint64 end = std::max(region.leaveNs, region.enterNs);
    m_maxNs = std::max(m_maxNs, end);
    if (!overlapsWindow(region.enterNs, end))
        return;
    // Pyramid-only mode: spans shorter than a level-1 bin are invisible in the overview.
    if (!m_keep && end - region.enterNs < m_minOverviewSpanNs)
        return;
    const int row = rowIndex(region.rank);
    if (row < 0)
        return;
    RankRow &r = m_store->rows[row];
    const quint8 depth = quint8(std::clamp(region.depth, 0, kMaxUserDepth));
    r.userSpans.append({region.enterNs, end, functionIndex(region.function), depth});
    r.maxDepth = std::max(r.maxDepth, depth);
}

// ---------------------------------------------------------------------------------------
// Listener

void TimelineBuilder::patchWait(int row, const P2PRecv &recv, qint64 lateNs)
{
    const qint64 duration = std::max<qint64>(0, recv.leaveNs - recv.enterNs);
    const qint64 wait = std::min(lateNs, duration);
    if (wait <= 0)
        return;
    qint64 oldWait = 0;
    Category cat = Category::Recv;
    bool known = false;
    if (m_keep) {
        const int idx = findActivity(row, recv.enterNs, recv.leaveNs);
        if (idx >= 0) {
            Activity &a = m_store->rows[row].activities[idx];
            oldWait = a.waitNs;
            cat = a.category;
            a.waitNs = std::max(a.waitNs, wait);
            known = true;
        }
    }
    if (!m_pyramidOn)
        return;
    if (!known) {
        // Activities not kept: remember the waits of the last calls that completed receives
        // (matches of one MPI_Waitall arrive right after each other).
        RowState &st = m_rowState[row];
        RecentWait *hit = nullptr;
        for (RecentWait &w : st.recentWaits) {
            if (w.startNs == recv.enterNs && w.endNs == recv.leaveNs) {
                hit = &w;
                break;
            }
        }
        if (hit) {
            oldWait = hit->waitNs;
            hit->waitNs = std::max(hit->waitNs, wait);
        } else if (st.recentWaits.size() < kRecentWaits) {
            st.recentWaits.append({recv.enterNs, recv.leaveNs, wait});
        } else {
            st.recentWaits[st.recentNext] = {recv.enterNs, recv.leaveNs, wait};
            st.recentNext = (st.recentNext + 1) % kRecentWaits;
        }
    }
    if (wait > oldWait)
        addToPyramid(row, recv.enterNs + oldWait, recv.enterNs + wait, cat, true);
}

void TimelineBuilder::addMessage(const P2PSend *send, const P2PRecv *recv, double lateSender, double lateReceiver)
{
    if (!m_keep)
        return;
    // Kept if the message's span overlaps the window; a receive counts from its enter (the
    // receive call is aggregated by its enter), the arrow ends at its leave.
    const qint64 sendNs = send ? send->enterNs : -1;
    const qint64 recvNs = recv ? recv->leaveNs : -1;
    qint64 lo = send ? send->enterNs : recv->enterNs;
    qint64 hi = lo;
    if (recv) {
        lo = std::min(lo, recv->enterNs);
        hi = std::max(hi, recv->leaveNs);
    }
    if (!overlapsWindow(lo, hi))
        return;
    const int idx = int(m_store->messages.size());
    const P2PSend &ref = send ? *send : P2PSend{};
    Message m{};
    m.sender = send ? send->rank : recv->peer;
    m.receiver = recv ? recv->rank : -1;
    m.sendNs = sendNs;
    m.recvNs = recvNs;
    m.bytes = send ? send->bytes : recv->bytes;
    m.tag = send ? send->tag : recv->tag;
    m.comm = send ? send->comm : recv->comm;
    m.lateSender = lateSender;
    m.lateReceiver = lateReceiver;
    m.sendFunction = send ? functionIndex(ref.function) : functionIndex(recv->function);
    m.recvFunction = recv ? functionIndex(recv->function) : m.sendFunction;
    m.destination = send ? send->peer : recv->rank;
    m_store->messages.append(m);
    if (send) {
        const int sRow = rowIndex(send->rank);
        if (sRow >= 0)
            m_rowState[sRow].links.append({send->enterNs, idx});
    }
    if (recv) {
        const int rRow = rowIndex(recv->rank);
        if (rRow >= 0)
            m_rowState[rRow].links.append({recv->enterNs, idx});
    }
}

void TimelineBuilder::onMatched(const MatchedMessage &match)
{
    const int rRow = rowIndex(match.recv.rank);
    if (rRow >= 0)
        patchWait(rRow, match.recv, std::max<qint64>(0, match.send.enterNs - match.recv.enterNs));
    addMessage(&match.send, &match.recv, match.lateSender, match.lateReceiver);
}

void TimelineBuilder::onUnmatchedSend(const P2PSend &send)
{
    ++m_store->unmatchedMessages;
    addMessage(&send, nullptr, 0.0, 0.0);
}

void TimelineBuilder::onUnmatchedRecv(const P2PRecv &recv)
{
    ++m_store->unmatchedMessages;
    addMessage(nullptr, &recv, 0.0, 0.0);
}

void TimelineBuilder::onCollective(const CompletedCollective &coll)
{
    if (!m_keep)
        return;
    for (int i = 0; i < coll.members.size(); ++i) {
        const Collective &c = coll.members[i];
        if (!overlapsWindow(c.enterNs, c.leaveNs))
            continue;
        const int row = rowIndex(c.rank);
        if (row < 0)
            continue;
        // Share one member list per communicator.
        QVector<int> &members = m_commMembers[c.comm];
        if (members.constData() != c.members.constData() && members != c.members)
            members = c.members;
        const int idx = int(m_store->collectives.size());
        m_store->collectives.append({c.rank, c.comm, members, c.bytesSent, c.bytesRecv,
                                     i < coll.lateSender.size() ? coll.lateSender[i] : 0.0});
        // Collective links are stored as -(index + 1) until finish().
        m_rowState[row].links.append({c.enterNs, -(idx + 1)});
    }
}

// ---------------------------------------------------------------------------------------
// Finish

std::shared_ptr<TimelineStore> TimelineBuilder::finish()
{
    std::shared_ptr<TimelineStore> store = std::move(m_store);
    m_store = std::make_shared<TimelineStore>();
    m_agg.setListener(nullptr);
    if (!store)
        return {};

    const qint64 len = std::max<qint64>(0, m_maxNs);
    store->traceLengthNs = len;
    const bool fullRange = m_from <= 0 && m_to >= len;
    store->complete = m_keep && fullRange;
    store->fromNs = fullRange ? 0 : std::clamp<qint64>(m_from, 0, len);
    store->toNs = fullRange ? len : std::clamp<qint64>(m_to, store->fromNs, len);

    // Messages by sendNs (ties: sender, receiver, tag for a deterministic order).
    QVector<int> order(store->messages.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&store](int a, int b) {
        const Message &x = store->messages[a];
        const Message &y = store->messages[b];
        return std::tie(x.sendNs, x.sender, x.receiver, x.tag) < std::tie(y.sendNs, y.sender, y.receiver, y.tag);
    });
    QVector<qint32> newIndex(order.size());
    QVector<Message> sorted;
    sorted.reserve(order.size());
    for (int i = 0; i < order.size(); ++i) {
        newIndex[order[i]] = i;
        sorted.append(store->messages[order[i]]);
    }
    store->messages = std::move(sorted);

    for (int r = 0; r < store->rows.size(); ++r) {
        RankRow &row = store->rows[r];
        QVector<PendingLink> &pending = m_rowState[r].links;
        std::stable_sort(pending.begin(), pending.end(),
                         [](const PendingLink &a, const PendingLink &b) { return a.startNs < b.startNs; });
        // Merge walk: links and activities are both sorted by start; links of one activity
        // end up contiguous.
        row.links.reserve(pending.size());
        int ai = 0;
        for (const PendingLink &l : pending) {
            while (ai < row.activities.size() && row.activities[ai].startNs < l.startNs)
                ++ai;
            if (ai >= row.activities.size())
                break;
            Activity &a = row.activities[ai];
            if (a.startNs != l.startNs || a.linkCount == 65535)
                continue; // activity outside the window (or not an MPI region)
            if (a.linkCount == 0)
                a.firstLink = qint32(row.links.size());
            row.links.append(l.target >= 0 ? newIndex[l.target] : -(l.target + 1));
            ++a.linkCount;
        }
        pending = {};
        m_rowState[r].recentWaits = {};

        std::sort(row.userSpans.begin(), row.userSpans.end(), [](const UserSpan &a, const UserSpan &b) {
            return a.startNs != b.startNs ? a.startNs < b.startNs : a.depth < b.depth;
        });
        buildSpanIndex(row, store->traceLengthNs);
    }

    for (int i = 0; i < store->messages.size(); ++i) {
        const Message &m = store->messages[i];
        const int s = m.sendNs < 0 ? -1 : store->rowIndexOfRank(m.sender); // unmatched receive: no send side
        if (s >= 0)
            store->rows[s].messages.append(i);
        if (m.receiver >= 0 && m.receiver != m.sender) {
            const int rr = store->rowIndexOfRank(m.receiver);
            if (rr >= 0)
                store->rows[rr].messages.append(i);
        }
    }

    // Compute = remainder of every pyramid bin.
    Pyramid &p = store->pyramid;
    for (int k = 0; k < p.levels; ++k) {
        for (QVector<quint16> &bins : p.data[k]) {
            quint16 *d = bins.data();
            for (qsizetype b = 0; b < bins.size(); b += kCategoryCount) {
                int others = 0;
                for (int c = 1; c < kCategoryCount; ++c)
                    others += d[b + c];
                d[b + catIndex(Category::Compute)] = quint16(std::max(0, 65535 - others));
            }
        }
    }

    for (RankRow &row : store->rows) {
        row.activities.squeeze();
        row.links.squeeze();
        row.messages.squeeze();
        row.userSpans.squeeze();
    }
    store->messages.squeeze();
    store->collectives.squeeze();
    m_rowState.clear();
    m_commMembers.clear();
    m_functionIndex.clear();
    return store;
}

} // namespace trace
