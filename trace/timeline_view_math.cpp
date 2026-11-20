#include "timeline_view_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace trace {

namespace {

constexpr qint64 kNsPerUs = 1000;
constexpr qint64 kNsPerMs = 1'000'000;
constexpr qint64 kNsPerS = 1'000'000'000;

// Maps times to fractional column positions of a window.
struct Columns {
    qint64 from;
    double width; // ns per column
    int count;

    double pos(qint64 ns) const { return double(ns - from) / width; }
};

// Calls f(column, overlapNs) for every column overlapped by [a, b).
template<typename F>
void forEachColumn(const Columns &c, qint64 a, qint64 b, F f)
{
    if (b <= a)
        return;
    double pa = std::max(0.0, c.pos(a));
    const double pb = std::min(double(c.count), c.pos(b));
    while (pa < pb) {
        const int col = std::min(c.count - 1, int(pa));
        const double end = std::min(pb, double(col + 1));
        if (end > pa)
            f(col, (end - pa) * c.width);
        if (col == c.count - 1)
            break;
        pa = double(col + 1);
    }
}

bool coversWindow(const TimelineStore &s, qint64 from, qint64 to)
{
    return s.complete || (s.fromNs <= from && s.toNs >= to);
}

// Picks the category with the largest share; MPI categories win ties against Compute.
ColumnSample pick(const double *share)
{
    int best = int(Category::Compute);
    double bestValue = share[best];
    for (int c = 1; c < kCategoryCount; ++c) {
        if (share[c] > 0 && share[c] >= bestValue && (share[c] > bestValue || best == int(Category::Compute))) {
            best = c;
            bestValue = share[c];
        }
    }
    return {Category(best), share[int(Category::Wait)] > 0};
}

void sampleActivities(const RankRow &row, const Columns &c, qint64 to, QVector<ColumnSample> &out)
{
    QVector<double> share(qsizetype(c.count) * kCategoryCount, 0.0);
    const QVector<Activity> &acts = row.activities;
    auto it = std::lower_bound(acts.begin(), acts.end(), c.from,
                               [](const Activity &a, qint64 ns) { return a.startNs < ns; });
    qsizetype i = it - acts.begin();
    while (i > 0 && acts[i - 1].endNs > c.from) // calls running into the window
        --i;
    double *data = share.data();
    for (; i < acts.size() && acts[i].startNs < to; ++i) {
        const Activity &a = acts[i];
        const qint64 waitEnd = std::min(a.endNs, a.startNs + std::max<qint64>(0, a.waitNs));
        const int wait = int(Category::Wait);
        const int cat = int(a.category);
        forEachColumn(c, a.startNs, waitEnd, [&](int col, double ns) { data[col * kCategoryCount + wait] += ns; });
        forEachColumn(c, waitEnd, a.endNs, [&](int col, double ns) { data[col * kCategoryCount + cat] += ns; });
    }
    for (int col = 0; col < c.count; ++col) {
        double *s = data + col * kCategoryCount;
        double covered = 0;
        for (int k = 1; k < kCategoryCount; ++k)
            covered += s[k];
        s[int(Category::Compute)] = std::max(0.0, c.width - covered);
        out[col] = pick(s);
    }
}

void samplePyramid(const Pyramid &p, int rowIndex, const Columns &c, QVector<ColumnSample> &out)
{
    // Widest bins still <= the column width; the finest level if all are wider.
    int level = p.levels - 1;
    for (int k = 0; k < p.levels; ++k) {
        if (double(p.traceLengthNs) / p.binsPerLevel[k] <= c.width) {
            level = k;
            break;
        }
    }
    const qint64 bins = p.binsPerLevel[level];
    const double binWidth = double(p.traceLengthNs) / double(bins);
    const quint16 *data = p.data[level][rowIndex].constData();
    for (int col = 0; col < c.count; ++col) {
        const double c0 = double(c.from) + col * c.width;
        const double c1 = c0 + c.width;
        const qint64 b0 = std::max<qint64>(0, qint64(std::floor(c0 / binWidth)));
        const qint64 b1 = std::min<qint64>(bins, qint64(std::ceil(c1 / binWidth)));
        double share[kCategoryCount] = {};
        for (qint64 b = b0; b < b1; ++b) {
            const double overlap = std::min(c1, (b + 1) * binWidth) - std::max(c0, b * binWidth);
            if (overlap <= 0)
                continue;
            const quint16 *bin = data + b * kCategoryCount;
            for (int k = 0; k < kCategoryCount; ++k)
                share[k] += overlap * bin[k];
        }
        out[col] = pick(share);
    }
}

bool hasSpanIndex(const RankRow &row)
{
    const qsizetype depths = row.userSpans.isEmpty() ? 0 : row.maxDepth + 1;
    return row.spansByDepth.size() == depths && row.spansByDepthCoarse.size() == depths;
}

// The row itself if its span index is built, else an indexed copy in tmp (stores not made by
// TimelineBuilder, e.g. hand-built in tests).
const RankRow &indexedRow(const RankRow &row, qint64 traceLengthNs, RankRow &tmp)
{
    if (hasSpanIndex(row))
        return row;
    tmp.userSpans = row.userSpans;
    tmp.maxDepth = row.maxDepth;
    buildSpanIndex(tmp, traceLengthNs);
    return tmp;
}

// Open user spans at ns with depth <= maxDepth, by depth (index = depth, -1 = none): per depth
// the last span starting at or before ns, if still running (spans of one depth do not overlap;
// at the capped depth a few earlier ones are checked too). O(depth * log spans).
void openSpansAt(const RankRow &row, qint64 ns, int maxDepth, std::array<int, kMaxUserDepth + 1> &open)
{
    open.fill(-1);
    const int limit = std::min<int>({maxDepth, int(row.maxDepth), int(row.spansByDepth.size()) - 1});
    const QVector<UserSpan> &spans = row.userSpans;
    for (int d = 0; d <= limit; ++d) {
        const QVector<qint32> &list = row.spansByDepth[d];
        auto it = std::upper_bound(list.begin(), list.end(), ns,
                                   [&](qint64 t, qint32 i) { return t < spans[i].startNs; });
        const qsizetype last = (it - list.begin()) - 1;
        const qsizetype stop = d == kMaxUserDepth ? std::max<qsizetype>(-1, last - 64) : last - 1;
        for (qsizetype k = last; k > stop && k >= 0; --k) {
            if (spans[list[k]].endNs > ns) {
                open[d] = list[k];
                break;
            }
        }
    }
}

int phaseOf(const RankRow &row, const std::array<int, kMaxUserDepth + 1> &open, PhaseLevel level)
{
    if (level == PhaseLevel::Outermost) {
        const int i = open[1] >= 0 ? open[1] : open[0];
        return i >= 0 ? row.userSpans[i].function : -1;
    }
    for (int d = kMaxUserDepth; d >= 0; --d)
        if (open[d] >= 0)
            return row.userSpans[open[d]].function;
    return -1;
}

QString unitLabel(qint64 ns, qint64 step)
{
    qint64 unit = kNsPerUs;
    QString name = QStringLiteral("µs");
    if (step >= kNsPerS) {
        unit = kNsPerS;
        name = QStringLiteral("s");
    } else if (step >= kNsPerMs) {
        unit = kNsPerMs;
        name = QStringLiteral("ms");
    }
    const QString value = step % unit == 0 ? QString::number(ns / unit)
                                           : QString::number(double(ns) / double(unit), 'f', 1);
    return value + QLatin1Char(' ') + name;
}

} // namespace

QVector<AxisTick> axisTicks(qint64 fromNs, qint64 toNs, double widthPx, double minSpacingPx)
{
    QVector<AxisTick> ticks;
    if (toNs <= fromNs || !(widthPx > 0))
        return ticks;
    const double span = double(toNs - fromNs);
    minSpacingPx = std::max(1.0, minSpacingPx); // >= 1 px apart: at most widthPx + 1 ticks
    const qsizetype maxTicks = qsizetype(std::min(widthPx, 1e6)) + 2;
    qint64 step = 0;
    for (qint64 decade = 1; step == 0; decade *= 10) {
        for (qint64 m : {1, 2, 5}) {
            if (double(decade * m) / span * widthPx >= minSpacingPx) {
                step = decade * m;
                break;
            }
        }
        if (decade > std::numeric_limits<qint64>::max() / 50)
            break;
    }
    if (step == 0)
        step = toNs - fromNs; // at most one tick: the window start
    qint64 t = fromNs >= 0 ? (fromNs + step - 1) / step * step : fromNs / step * step;
    for (; t <= toNs && ticks.size() < maxTicks; t += step) {
        ticks.append({t, double(t - fromNs) * widthPx / span, unitLabel(t, step)});
        if (t > std::numeric_limits<qint64>::max() - step)
            break;
    }
    return ticks;
}

QVector<ColumnSample> sampleRow(const TimelineStore &overview, const TimelineStore *detail, int rowIndex,
                                qint64 fromNs, qint64 toNs, int columns)
{
    if (rowIndex < 0 || rowIndex >= overview.rows.size() || columns <= 0 || toNs <= fromNs)
        return {};
    QVector<ColumnSample> out(columns, ColumnSample{Category::Compute, false});
    const Columns c{fromNs, double(toNs - fromNs) / double(columns), columns};
    const Pyramid &p = overview.pyramid;
    const bool hasPyramid = p.levels > 0 && p.traceLengthNs > 0 && rowIndex < p.data[0].size();
    const double finestBin = hasPyramid ? double(p.traceLengthNs) / p.binsPerLevel[p.levels - 1] : 0;

    if (!hasPyramid || c.width < finestBin) {
        const TimelineStore *raw = nullptr;
        if (detail && coversWindow(*detail, fromNs, toNs))
            raw = detail;
        else if (overview.complete)
            raw = &overview;
        if (raw) {
            const int r = raw == &overview ? rowIndex : raw->rowIndexOfRank(overview.rows[rowIndex].rank);
            if (r >= 0)
                sampleActivities(raw->rows[r], c, toNs, out);
            return out;
        }
    }
    if (hasPyramid)
        samplePyramid(p, rowIndex, c, out);
    return out;
}

namespace {

// See HitResult::completes.
QStringList completedCalls(const TimelineStore &detail, const RankRow &row, const Activity &a, const QString &own)
{
    QStringList keys;
    QVector<int> counts;
    const qint64 end = qint64(a.firstLink) + a.linkCount;
    for (qint64 i = std::max<qint64>(0, a.firstLink); i < end && i < row.links.size(); ++i) {
        const int target = row.links[i];
        if (target < 0 || target >= detail.messages.size())
            continue;
        const Message &m = detail.messages[target];
        const bool isSend = m.sender == row.rank && m.sendNs == a.startNs;
        const QString fn = detail.functions.value(isSend ? m.sendFunction : m.recvFunction);
        if (fn.isEmpty() || fn == own)
            continue;
        const QString key = isSend
            ? QStringLiteral("%1 to rank %2").arg(fn).arg(m.receiver >= 0 ? m.receiver : m.destination)
            : QStringLiteral("%1 from rank %2").arg(fn).arg(m.sender);
        const qsizetype k = keys.indexOf(key);
        if (k < 0) {
            keys.append(key);
            counts.append(1);
        } else {
            ++counts[k];
        }
    }
    for (qsizetype k = 0; k < keys.size(); ++k)
        if (counts[k] > 1)
            keys[k] = QStringLiteral("%1× %2").arg(counts[k]).arg(keys[k]);
    return keys;
}

} // namespace

HitResult activityAt(const TimelineStore &detail, int rowIndex, qint64 ns)
{
    HitResult h;
    if (rowIndex < 0 || rowIndex >= detail.rows.size() || ns < 0 || ns > detail.traceLengthNs)
        return h;
    if (!detail.complete && (ns < detail.fromNs || ns > detail.toNs))
        return h;
    const RankRow &row = detail.rows[rowIndex];
    const qint64 lo = detail.complete ? 0 : std::max<qint64>(0, detail.fromNs);
    const qint64 hi = detail.complete ? detail.traceLengthNs : std::min(detail.traceLengthNs, detail.toNs);
    h.valid = true;
    h.rank = row.rank;

    const QVector<Activity> &acts = row.activities;
    auto it = std::upper_bound(acts.begin(), acts.end(), ns,
                               [](qint64 t, const Activity &a) { return t < a.startNs; });
    const qsizetype next = it - acts.begin();
    // Innermost containing call: the latest-starting one still running at ns. MPI calls nest
    // rarely and shallowly, so a short backward scan suffices.
    constexpr qsizetype kNestScan = 64;
    const Activity *hit = nullptr;
    qint64 gapStart = lo;
    for (qsizetype i = next - 1; i >= 0 && i >= next - kNestScan; --i) {
        const Activity &a = acts[i];
        if (a.endNs > ns) {
            hit = &a;
            break;
        }
        gapStart = std::max(gapStart, a.endNs);
    }

    if (hit) {
        h.function = detail.functions.value(hit->function);
        h.startNs = hit->startNs;
        h.durationNs = hit->endNs - hit->startNs;
        h.waitNs = hit->waitNs;
        h.messageCount = hit->linkCount;
        if (hit->linkCount > 0 && hit->firstLink >= 0 && hit->firstLink < row.links.size()) {
            const int target = row.links[hit->firstLink];
            if (hit->category == Category::Collective) {
                if (target >= 0 && target < detail.collectives.size()) {
                    const CollectiveEntry &e = detail.collectives[target];
                    h.bytes = e.bytesSent;
                    h.comm = e.comm;
                }
            } else if (target >= 0 && target < detail.messages.size()) {
                const Message &m = detail.messages[target];
                const bool isSend = m.sender == row.rank && m.sendNs == hit->startNs;
                h.partner = isSend ? (m.receiver >= 0 ? m.receiver : m.destination) : m.sender;
                h.bytes = m.bytes;
                h.tag = m.tag;
                h.comm = m.comm;
            }
        }
        if (hit->category != Category::Collective)
            h.completes = completedCalls(detail, row, *hit, h.function);
    } else {
        h.function = QStringLiteral("Computation");
        h.startNs = gapStart;
        const qint64 gapEnd = next < acts.size() ? std::min(hi, acts[next].startNs) : hi;
        h.durationNs = std::max<qint64>(0, gapEnd - gapStart);
    }

    RankRow tmp;
    const RankRow &spanRow = indexedRow(row, detail.traceLengthNs, tmp);
    std::array<int, kMaxUserDepth + 1> open;
    openSpansAt(spanRow, ns, kMaxUserDepth, open);
    for (int d = 0; d <= kMaxUserDepth; ++d)
        if (open[d] >= 0)
            h.callPath.append(detail.functions.value(spanRow.userSpans[open[d]].function));
    return h;
}

int phaseFunctionAt(const TimelineStore &store, int rowIndex, qint64 ns, PhaseLevel level)
{
    if (rowIndex < 0 || rowIndex >= store.rows.size())
        return -1;
    RankRow tmp;
    const RankRow &row = indexedRow(store.rows[rowIndex], store.traceLengthNs, tmp);
    std::array<int, kMaxUserDepth + 1> open;
    openSpansAt(row, ns, level == PhaseLevel::Outermost ? 1 : kMaxUserDepth, open);
    return phaseOf(row, open, level);
}

namespace {

// Turns the spans of a row into time-ordered segments (shown function per time) and books them
// per column: fill(d, a, b, parent) shows `parent` in [a, b) except where depth-d spans (and,
// recursively, their children up to maxDepth) run. Only spans overlapping the window are visited.
struct BandSampler {
    const QVector<UserSpan> &spans;
    const QVector<QVector<qint32>> &lists; // per depth, fine or coarse
    int maxDepth;
    Columns c;
    qint64 toNs;
    QVector<int> &out;

    static constexpr int kEntries = 16;
    std::array<std::pair<int, double>, kEntries> entries{};
    int used = 0;
    int current = -1;

    void finish()
    {
        if (current < 0 || used == 0)
            return;
        int best = 0;
        for (int i = 1; i < used; ++i)
            if (entries[i].second > entries[best].second)
                best = i;
        out[current] = entries[best].first;
    }

    void addSegment(qint64 a, qint64 b, int function)
    {
        forEachColumn(c, std::max(a, c.from), std::min(b, toNs), [&](int col, double ns) {
            if (col != current) {
                finish();
                current = col;
                used = 0;
            }
            for (int i = 0; i < used; ++i) {
                if (entries[i].first == function) {
                    entries[i].second += ns;
                    return;
                }
            }
            if (used < kEntries)
                entries[used++] = {function, ns};
        });
    }

    void fill(int d, qint64 a, qint64 b, int parent)
    {
        if (d > maxDepth || d >= lists.size()) {
            addSegment(a, b, parent);
            return;
        }
        const QVector<qint32> &list = lists[d];
        auto it = std::upper_bound(list.begin(), list.end(), a,
                                   [&](qint64 t, qint32 i) { return t < spans[i].startNs; });
        qsizetype k = std::max<qsizetype>(0, (it - list.begin()) - 1);
        qint64 pos = a;
        for (; k < list.size(); ++k) {
            const UserSpan &u = spans[list[k]];
            if (u.startNs >= b)
                break;
            const qint64 s0 = std::max(u.startNs, pos);
            const qint64 s1 = std::min(u.endNs, b);
            if (s1 <= s0)
                continue; // ended before pos (or overlapping sibling at the capped depth)
            if (s0 > pos)
                addSegment(pos, s0, parent);
            fill(d + 1, s0, s1, u.function);
            pos = s1;
        }
        if (pos < b)
            addSegment(pos, b, parent);
    }
};

} // namespace

QVector<int> samplePhaseBand(const TimelineStore &store, int rowIndex, qint64 fromNs, qint64 toNs, int columns,
                             PhaseLevel level)
{
    if (rowIndex < 0 || rowIndex >= store.rows.size() || columns <= 0 || toNs <= fromNs)
        return {};
    QVector<int> out(columns, -1);
    RankRow tmp;
    const RankRow &row = indexedRow(store.rows[rowIndex], store.traceLengthNs, tmp);
    const Columns c{fromNs, double(toNs - fromNs) / double(columns), columns};
    // Zoomed out (columns at least a level-1 pyramid bin wide) only spans of that length count,
    // like in the pyramid-only overview; this bounds the work independent of the span count.
    const qint64 coarse = coarseSpanNs(store.traceLengthNs);
    const bool useCoarse = coarse > 0 && c.width >= double(coarse);
    BandSampler sampler{row.userSpans, useCoarse ? row.spansByDepthCoarse : row.spansByDepth,
                        level == PhaseLevel::Outermost ? 1 : kMaxUserDepth, c, toNs, out};
    sampler.fill(0, fromNs, toNs, -1);
    sampler.finish();
    return out;
}

QVector<int> messagesInView(const TimelineStore &detail, int rank, qint64 fromNs, qint64 toNs, int cap)
{
    QVector<int> out;
    if (cap <= 0 || toNs < fromNs)
        return out;
    auto drawable = [&](int idx) {
        const Message &m = detail.messages[idx];
        return m.sendNs >= 0 && m.receiver >= 0 && m.sendNs <= toNs && m.recvNs >= fromNs;
    };
    auto sendOf = [&](int idx) { return detail.messages[idx].sendNs; };

    if (rank < 0) {
        auto it = std::lower_bound(detail.messages.begin(), detail.messages.end(), fromNs,
                                   [](const Message &m, qint64 t) { return m.sendNs < t; });
        const qsizetype first = std::max<qsizetype>(0, (it - detail.messages.begin()) - kMessageLookback);
        for (qsizetype i = first; i < detail.messages.size() && out.size() < cap; ++i) {
            if (detail.messages[i].sendNs > toNs)
                break;
            if (drawable(int(i)))
                out.append(int(i));
        }
        return out;
    }
    const int r = detail.rowIndexOfRank(rank);
    if (r < 0)
        return out;
    const QVector<qint32> &list = detail.rows[r].messages;
    auto it = std::lower_bound(list.begin(), list.end(), fromNs,
                               [&](qint32 idx, qint64 t) { return sendOf(idx) < t; });
    const qsizetype first = std::max<qsizetype>(0, (it - list.begin()) - kMessageLookback);
    for (qsizetype i = first; i < list.size() && out.size() < cap; ++i) {
        if (sendOf(list[i]) > toNs)
            break;
        if (drawable(list[i]))
            out.append(list[i]);
    }
    return out;
}

} // namespace trace
