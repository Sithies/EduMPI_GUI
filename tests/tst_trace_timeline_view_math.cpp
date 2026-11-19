#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <limits>

#include "otf2_fixture.h"
#include "otf2_trace_reader.h"
#include "timeline_builder.h"
#include "timeline_view_math.h"
#include "trace_aggregator.h"

using namespace trace;

namespace {

constexpr qint64 kMs = 1'000'000;
constexpr qint64 kAll = std::numeric_limits<qint64>::max();

std::shared_ptr<TimelineStore> build(const QString &anchor, qint64 from = 0, qint64 to = kAll, bool keep = true,
                                     int levels = 4)
{
    TraceAggregator agg;
    TimelineBuilder builder(agg, from, to, keep, levels);
    TraceMeta meta;
    QString error;
    if (!readOtf2(anchor, builder, &meta, &error)) {
        qWarning("%s", qPrintable(error));
        return {};
    }
    agg.finish(meta.traceLengthNs);
    return builder.finish();
}

int cat(const QVector<ColumnSample> &v, int column)
{
    return column < v.size() ? int(v[column].category) : -1;
}

QString fn(const TimelineStore &s, int id)
{
    return id < 0 ? QStringLiteral("<none>") : s.functions.value(id);
}

// Brute-force reference of the phase level at t over all spans of a row (optionally only those
// lasting at least minDuration): Outermost = depth 1 if open, else depth 0; Innermost = deepest.
int refPhaseAt(const RankRow &row, double t, PhaseLevel level, qint64 minDuration = 0)
{
    const int limit = level == PhaseLevel::Outermost ? 1 : kMaxUserDepth;
    int best = -1;
    int bestDepth = -1;
    for (const UserSpan &u : row.userSpans) {
        if (u.depth > limit || u.endNs - u.startNs < minDuration)
            continue;
        if (double(u.startNs) <= t && t < double(u.endNs) && u.depth > bestDepth) {
            best = u.function;
            bestDepth = u.depth;
        }
    }
    return best;
}

// Reference band: per column, the function with the largest coverage (first seen wins ties),
// using the coarse span set when the column is at least coarseSpanNs wide.
QVector<int> refBand(const TimelineStore &s, int rowIndex, qint64 from, qint64 to, int columns, PhaseLevel level)
{
    const RankRow &row = s.rows[rowIndex];
    const double w = double(to - from) / columns;
    const qint64 coarse = coarseSpanNs(s.traceLengthNs);
    const qint64 minDuration = coarse > 0 && w >= double(coarse) ? coarse : 0;
    QVector<int> out(columns, -1);
    for (int i = 0; i < columns; ++i) {
        const double c0 = double(from) + i * w;
        const double c1 = c0 + w;
        QVector<double> cuts{c0, c1};
        for (const UserSpan &u : row.userSpans) {
            if (u.endNs - u.startNs < minDuration)
                continue;
            if (u.startNs > c0 && u.startNs < c1)
                cuts.append(double(u.startNs));
            if (u.endNs > c0 && u.endNs < c1)
                cuts.append(double(u.endNs));
        }
        std::sort(cuts.begin(), cuts.end());
        QVector<QPair<int, double>> cover;
        for (int k = 0; k + 1 < cuts.size(); ++k) {
            const double len = cuts[k + 1] - cuts[k];
            if (len <= 0)
                continue;
            const int f = refPhaseAt(row, (cuts[k] + cuts[k + 1]) / 2, level, minDuration);
            auto it = std::find_if(cover.begin(), cover.end(), [f](const QPair<int, double> &p) { return p.first == f; });
            if (it == cover.end())
                cover.append({f, len});
            else
                it->second += len;
        }
        int best = 0;
        for (int k = 1; k < cover.size(); ++k)
            if (cover[k].second > cover[best].second)
                best = k;
        if (!cover.isEmpty())
            out[i] = cover[best].first;
    }
    return out;
}

// Random properly nested user regions below main [0, length] on rank 0.
void addNested(TimelineBuilder &b, QRandomGenerator &rng, int depth, qint64 a, qint64 e, int maxDepth)
{
    if (depth > maxDepth || e - a < 20)
        return;
    const int n = int(rng.bounded(0, 8));
    QVector<qint64> pts;
    for (int i = 0; i < 2 * n; ++i)
        pts.append(a + 1 + qint64(rng.bounded(quint64(e - a - 1))));
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    for (int i = 0; i + 1 < pts.size(); i += 2) {
        b.addUserRegion({0, QStringLiteral("f%1").arg(rng.bounded(12)), depth, pts[i], pts[i + 1]});
        addNested(b, rng, depth + 1, pts[i], pts[i + 1], maxDepth);
    }
}

std::shared_ptr<TimelineStore> randomNestedStore(quint32 seed, qint64 length)
{
    TraceAggregator agg;
    TimelineBuilder builder(agg, 0, kAll, true, 0);
    builder.addRank(0, QStringLiteral("n"));
    TraceMeta meta;
    meta.traceLengthNs = length;
    builder.begin(meta);
    builder.addUserRegion({0, QStringLiteral("main"), 0, 0, length});
    QRandomGenerator rng(seed);
    addNested(builder, rng, 1, 0, length, 7);
    agg.finish(length);
    return builder.finish();
}

} // namespace

class TestTimelineViewMath : public QObject
{
    Q_OBJECT

private slots:
    void ticksForRing4()
    {
        const QVector<AxisTick> t = axisTicks(0, 2'500'000'000, 1000);
        QCOMPARE(t.size(), 13);
        QCOMPARE(t[0].label, QStringLiteral("0 ms"));
        QCOMPARE(t[1].label, QStringLiteral("200 ms"));
        QCOMPARE(t[1].x, 80.0);
        QCOMPARE(t[1].ns, 200 * kMs);
        QCOMPARE(t[12].label, QStringLiteral("2400 ms"));
    }

    void ticksAtMicroseconds()
    {
        const QVector<AxisTick> t = axisTicks(1'000'000, 1'010'000, 1000);
        QCOMPARE(t.size(), 11);
        QCOMPARE(t[0].label, QStringLiteral("1000 µs"));
        QCOMPARE(t[0].x, 0.0);
        QCOMPARE(t[10].x, 1000.0);
    }

    void ticksInSecondsAndDecimals()
    {
        // 10 s over 400 px: 2 s step (80 px) -> "s" labels.
        const QVector<AxisTick> s = axisTicks(0, 10'000'000'000, 400);
        QCOMPARE(s.size(), 6);
        QCOMPARE(s[1].label, QStringLiteral("2 s"));
        // 500 ns step -> microseconds with one decimal.
        const QVector<AxisTick> u = axisTicks(0, 5000, 800);
        QCOMPARE(u[1].label, QStringLiteral("0.5 µs"));
        QVERIFY(axisTicks(10, 10, 100).isEmpty());
        QVERIFY(axisTicks(0, 10, 0).isEmpty());
    }

    void sampleRing4()
    {
        QTemporaryDir tmp;
        const auto s = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(s);
        const QVector<ColumnSample> r0 = sampleRow(*s, s.get(), s->rowIndexOfRank(0), 0, 2'500'000'000, 2500);
        QCOMPARE(r0.size(), 2500);
        QCOMPARE(cat(r0, 250), int(Category::Wait));
        QVERIFY(r0[250].hasWait);
        QCOMPARE(cat(r0, 100), int(Category::Send));
        QCOMPARE(cat(r0, 1100), int(Category::Collective));
        QCOMPARE(cat(r0, 50), int(Category::Compute));
        const QVector<ColumnSample> r1 = sampleRow(*s, s.get(), s->rowIndexOfRank(1), 0, 2'500'000'000, 2500);
        QCOMPARE(cat(r1, 250), int(Category::Compute));
        QVERIFY(!r1[250].hasWait);
    }

    void sampleUsesPyramidWhenZoomedOut()
    {
        QTemporaryDir tmp;
        const auto s = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))), 0, kAll, false, 4);
        QVERIFY(s);
        QVERIFY(s->rows[0].activities.isEmpty());
        const QVector<ColumnSample> r0 = sampleRow(*s, nullptr, s->rowIndexOfRank(0), 0, 2'500'000'000, 100);
        QCOMPARE(r0.size(), 100);
        QCOMPARE(cat(r0, 10), int(Category::Wait));
        QCOMPARE(cat(r0, 40), int(Category::Collective));
        QCOMPARE(cat(r0, 70), int(Category::Compute));
    }

    void sampleUsesDetailWhenZoomedIn()
    {
        // [99.5, 102.5] ms in 300 columns of 10 µs (< 38 µs finest bin): raw activities of a
        // windowed detail store next to a pyramid-only overview.
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.filePath(QStringLiteral("r4")));
        const auto overview = build(anchor, 0, kAll, false, 4);
        const auto detail = build(anchor, 50 * kMs, 150 * kMs, true, 0);
        QVERIFY(overview && detail);
        const qint64 from = 99'500'000;
        const qint64 to = 102'500'000;
        const QVector<ColumnSample> r0 = sampleRow(*overview, detail.get(), 0, from, to, 300);
        QCOMPARE(r0.size(), 300);
        QCOMPARE(cat(r0, 0), int(Category::Compute));  // [99.5, 99.51) ms
        QCOMPARE(cat(r0, 50), int(Category::Send));    // [100.0, 100.01) ms
        QCOMPARE(cat(r0, 200), int(Category::Wait));   // [101.5, 101.51) ms
        const QVector<ColumnSample> r1 = sampleRow(*overview, detail.get(), 1, from, to, 300);
        QCOMPARE(cat(r1, 200), int(Category::Recv));   // R1 MPI_Recv [101, 102] ms, no wait
        QCOMPARE(cat(r1, 280), int(Category::Compute));
        // No detail and no pyramid covering the window: all compute, never garbage.
        const QVector<ColumnSample> none = sampleRow(*detail, nullptr, 0, 0, 2'500'000'000, 10);
        QCOMPARE(none.size(), 10);
        QVERIFY(sampleRow(*overview, nullptr, -1, 0, 100, 10).isEmpty());
    }

    void activityAtGapReturnsComputation()
    {
        QTemporaryDir tmp;
        const auto s = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(s);
        const HitResult h = activityAt(*s, s->rowIndexOfRank(1), 250 * kMs);
        QVERIFY(h.valid);
        QCOMPARE(h.rank, 1);
        QCOMPARE(h.function, QStringLiteral("Computation"));
        QCOMPARE(h.startNs, 102 * kMs);
        QCOMPARE(h.durationNs, 898 * kMs);
        QCOMPARE(h.messageCount, 0);
        QCOMPARE(h.callPath, QStringList{QStringLiteral("main")});
    }

    void activityAtRecv()
    {
        QTemporaryDir tmp;
        const auto s = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(s);
        const HitResult h = activityAt(*s, s->rowIndexOfRank(0), 200 * kMs);
        QVERIFY(h.valid);
        QCOMPARE(h.function, QStringLiteral("MPI_Recv"));
        QCOMPARE(h.startNs, 101 * kMs);
        QCOMPARE(h.durationNs, 301 * kMs);
        QCOMPARE(h.waitNs, 299 * kMs);
        QCOMPARE(h.partner, 3);
        QCOMPARE(h.bytes, quint64(1000));
        QCOMPARE(h.tag, quint32(7));
        QCOMPARE(h.messageCount, 1);

        const HitResult send = activityAt(*s, s->rowIndexOfRank(0), 100 * kMs + 500'000);
        QCOMPARE(send.function, QStringLiteral("MPI_Send"));
        QCOMPARE(send.partner, 1);
        // Unmatched send R3 -> R0 at 2400 ms: partner = destination.
        const HitResult lost = activityAt(*s, s->rowIndexOfRank(3), 2400 * kMs + 500'000);
        QCOMPARE(lost.function, QStringLiteral("MPI_Send"));
        QCOMPARE(lost.partner, 0);
        QCOMPARE(lost.tag, quint32(11));
        QCOMPARE(lost.bytes, quint64(100));
        // Collective: bytes sent, no partner.
        const HitResult coll = activityAt(*s, s->rowIndexOfRank(0), 1100 * kMs);
        QCOMPARE(coll.function, QStringLiteral("MPI_Allreduce"));
        QCOMPARE(coll.partner, -1);
        QCOMPARE(coll.bytes, quint64(64));
    }

    void activityAtOutsideRowsIsEmpty()
    {
        QTemporaryDir tmp;
        const auto s = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(s);
        QVERIFY(!activityAt(*s, -1, 200 * kMs).valid);
        QVERIFY(!activityAt(*s, int(s->rows.size()), 200 * kMs).valid);
        QVERIFY(!activityAt(*s, 0, 2600 * kMs).valid);
        QVERIFY(!activityAt(*s, 0, -1).valid);
        // Windowed detail: outside the loaded window nothing is known.
        const auto w = build(writeRing4Otf2(tmp.filePath(QStringLiteral("w"))), 50 * kMs, 150 * kMs, true, 0);
        QVERIFY(w);
        QVERIFY(!activityAt(*w, 0, 1100 * kMs).valid);
        QVERIFY(activityAt(*w, 0, 100 * kMs + 1).valid);
    }

    void messagesInViewForRank()
    {
        QTemporaryDir tmp;
        const auto s = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(s);
        const QVector<int> m = messagesInView(*s, 0, 0, 500 * kMs, 100);
        QCOMPARE(m.size(), 2);
        QCOMPARE(s->messages[m[0]].sender, 0);
        QCOMPARE(s->messages[m[0]].receiver, 1);
        QCOMPARE(s->messages[m[1]].sender, 3);
        QCOMPARE(s->messages[m[1]].receiver, 0);
        QCOMPARE(messagesInView(*s, 0, 0, 500 * kMs, 1).size(), 1);
        QCOMPARE(messagesInView(*s, 0, 0, 500 * kMs, 1)[0], m[0]);
        // All ranks in second 0: the four ring messages; the unmatched send (2400 ms) never.
        QCOMPARE(messagesInView(*s, -1, 0, 1000 * kMs, 100).size(), 4);
        QCOMPARE(messagesInView(*s, -1, 2000 * kMs, 2500 * kMs, 100).size(), 2);
        // A message in flight across the whole window ([400, 402] ms around [401, 401.5] ms).
        QCOMPARE(messagesInView(*s, 0, 401 * kMs, 401 * kMs + 500'000, 100).size(), 1);
    }

    void phaseLevels()
    {
        QTemporaryDir tmp;
        const auto s = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))));
        QVERIFY(s);
        const int r0 = s->rowIndexOfRank(0);
        QCOMPARE(fn(*s, phaseFunctionAt(*s, r0, 550 * kMs, PhaseLevel::Outermost)), QStringLiteral("phase_a"));
        QCOMPARE(fn(*s, phaseFunctionAt(*s, r0, 550 * kMs, PhaseLevel::Innermost)), QStringLiteral("send_to"));
        QCOMPARE(fn(*s, phaseFunctionAt(*s, r0, 2500 * kMs, PhaseLevel::Outermost)), QStringLiteral("phase_b"));
        QCOMPARE(fn(*s, phaseFunctionAt(*s, r0, 2500 * kMs, PhaseLevel::Innermost)), QStringLiteral("phase_b"));
        QCOMPARE(fn(*s, phaseFunctionAt(*s, r0, 50 * kMs, PhaseLevel::Outermost)), QStringLiteral("main"));
        QCOMPARE(phaseFunctionAt(*s, r0, 3500 * kMs, PhaseLevel::Innermost), -1);
        QCOMPARE(phaseFunctionAt(*s, -1, 50 * kMs, PhaseLevel::Innermost), -1);
    }

    void callPathAtSendTo()
    {
        QTemporaryDir tmp;
        const auto s = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))));
        QVERIFY(s);
        const HitResult h = activityAt(*s, s->rowIndexOfRank(0), 500'500'000);
        QVERIFY(h.valid);
        QCOMPARE(h.function, QStringLiteral("MPI_Send"));
        QCOMPARE(h.callPath, (QStringList{QStringLiteral("main"), QStringLiteral("phase_a"), QStringLiteral("send_to")}));
    }

    void samplePhaseBandColumns()
    {
        QTemporaryDir tmp;
        const auto s = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))));
        QVERIFY(s);
        const int r0 = s->rowIndexOfRank(0);
        const QVector<int> out = samplePhaseBand(*s, r0, 0, 3'000'000'000, 30, PhaseLevel::Outermost);
        QCOMPARE(out.size(), 30);
        QCOMPARE(fn(*s, out[5]), QStringLiteral("phase_a"));
        QCOMPARE(fn(*s, out[20]), QStringLiteral("phase_b"));
        QCOMPARE(fn(*s, out[0]), QStringLiteral("main"));      // [0, 100) ms: before phase_a
        QCOMPARE(fn(*s, out[29]), QStringLiteral("main"));     // [2.9, 3.0) s: after phase_b
        const QVector<int> inner = samplePhaseBand(*s, r0, 0, 3'000'000'000, 30, PhaseLevel::Innermost);
        QCOMPARE(fn(*s, inner[5]), QStringLiteral("send_to"));
        QCOMPARE(fn(*s, inner[6]), QStringLiteral("phase_a"));
        // Window starting inside nested spans (initial open stack).
        const QVector<int> zoom = samplePhaseBand(*s, r0, 550 * kMs, 650 * kMs, 10, PhaseLevel::Innermost);
        QCOMPARE(fn(*s, zoom[0]), QStringLiteral("send_to"));
        QCOMPARE(fn(*s, zoom[9]), QStringLiteral("phase_a"));
        // Past the trace end nothing is open.
        const QVector<int> past = samplePhaseBand(*s, r0, 3000 * kMs, 4000 * kMs, 4, PhaseLevel::Outermost);
        QCOMPARE(past, (QVector<int>{-1, -1, -1, -1}));
    }
    void ticksWithZeroSpacingAreBounded()
    {
        QElapsedTimer timer;
        timer.start();
        const QVector<AxisTick> t = axisTicks(0, 10'000'000'000, 1000, 0);
        QVERIFY2(timer.elapsed() < 1000, "axisTicks must not enumerate nanoseconds");
        QVERIFY(!t.isEmpty());
        QVERIFY(t.size() <= 1002);
        QVERIFY(axisTicks(0, 10'000'000'000, 1000, -5).size() <= 1002);
    }

    void spanIndexPerDepth()
    {
        QTemporaryDir tmp;
        const auto s = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))));
        QVERIFY(s);
        const RankRow &r = s->rows[s->rowIndexOfRank(0)];
        QCOMPARE(r.spansByDepth.size(), 3);
        QCOMPARE(r.spansByDepth[0].size(), 1);
        QCOMPARE(r.spansByDepth[1].size(), 2); // phase_a, phase_b
        QCOMPARE(r.spansByDepth[2].size(), 1);
        QCOMPARE(fn(*s, r.userSpans[r.spansByDepth[1][1]].function), QStringLiteral("phase_b"));
        QCOMPARE(r.spansByDepthCoarse.size(), 3); // all phases spans exceed 3 s / 4096
        QCOMPARE(r.spansByDepthCoarse[1].size(), 2);
    }

    void phaseLookupIsBounded()
    {
        // 1 rank, main + 200 000 depth-1 children of 5 us every 10 us (shorter than 2 s / 4096).
        constexpr qint64 n = 200'000;
        constexpr qint64 length = n * 10'000;
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, true, 0);
        builder.addRank(0, QStringLiteral("n"));
        TraceMeta meta;
        meta.traceLengthNs = length;
        builder.begin(meta);
        builder.addUserRegion({0, QStringLiteral("main"), 0, 0, length});
        const QString child = QStringLiteral("child");
        for (qint64 i = 0; i < n; ++i)
            builder.addUserRegion({0, child, 1, i * 10'000, i * 10'000 + 5'000});
        agg.finish(length);
        const auto s = builder.finish();
        QVERIFY(s);
        QCOMPARE(s->rows[0].userSpans.size(), n + 1);

        QElapsedTimer timer;
        timer.start();
        int hits = 0;
        for (int i = 0; i < 256; ++i)
            hits += fn(*s, phaseFunctionAt(*s, 0, length - 7'000 - i, PhaseLevel::Innermost)) == child;
        const qint64 lookupMs = timer.elapsed();
        QCOMPARE(hits, 256); // [length - 10 us, length - 5 us) is the last child
        timer.restart();
        const QVector<int> band = samplePhaseBand(*s, 0, 0, length, 1500, PhaseLevel::Outermost);
        const qint64 bandMs = timer.elapsed();
        timer.restart();
        for (int i = 0; i < 256; ++i)
            samplePhaseBand(*s, 0, 0, length, 1500, PhaseLevel::Innermost);
        const qint64 frameMs = timer.elapsed();
        qInfo("256 lookups %lld ms, one band %lld ms, 256 bands %lld ms", lookupMs, bandMs, frameMs);
        QCOMPARE(fn(*s, band[700]), QStringLiteral("main")); // children below the coarse threshold
        QVERIFY2(lookupMs < 50, "phaseFunctionAt must not scan all earlier spans");
        QVERIFY2(bandMs < 50, "zoomed-out band must not walk every span");
        QVERIFY2(frameMs < 100, "256 zoomed-out bands (one frame) must stay bounded");
        // Zoomed in, the children are visible again.
        const QVector<int> zoom = samplePhaseBand(*s, 0, 1'000'000, 1'100'000, 20, PhaseLevel::Outermost);
        QCOMPARE(fn(*s, zoom[0]), child);                 // [1.000, 1.005) ms
        QCOMPARE(fn(*s, zoom[1]), QStringLiteral("main")); // [1.005, 1.010) ms
        QCOMPARE(fn(*s, zoom[2]), child);
    }

    void phaseLevelsMatchReferenceOnPhases()
    {
        QTemporaryDir tmp;
        const auto s = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))));
        QVERIFY(s);
        for (int rank : {0, 1}) {
            const int r = s->rowIndexOfRank(rank);
            for (PhaseLevel level : {PhaseLevel::Outermost, PhaseLevel::Innermost}) {
                for (qint64 t = 0; t <= 3100 * kMs; t += kMs / 2)
                    QCOMPARE(phaseFunctionAt(*s, r, t, level), refPhaseAt(s->rows[r], double(t), level));
                QCOMPARE(samplePhaseBand(*s, r, 0, 3'000'000'000, 30, level), refBand(*s, r, 0, 3'000'000'000, 30, level));
                QCOMPARE(samplePhaseBand(*s, r, 450 * kMs, 650 * kMs, 1000, level),
                         refBand(*s, r, 450 * kMs, 650 * kMs, 1000, level));
            }
        }
    }

    void phaseLevelsMatchReferenceOnRandomNesting()
    {
        constexpr qint64 length = 1'000'000'000;
        for (quint32 seed : {1u, 7u, 42u}) {
            const auto s = randomNestedStore(seed, length);
            QVERIFY(s);
            const RankRow &row = s->rows[0];
            QVERIFY(row.userSpans.size() > 100);
            QRandomGenerator rng(seed + 1000);
            for (PhaseLevel level : {PhaseLevel::Outermost, PhaseLevel::Innermost}) {
                for (int k = 0; k < 2000; ++k) {
                    const qint64 t = qint64(rng.bounded(quint64(length + 1000)));
                    QCOMPARE(phaseFunctionAt(*s, 0, t, level), refPhaseAt(row, double(t), level));
                }
                const HitResult h = activityAt(*s, 0, length / 3);
                QCOMPARE(h.callPath.size(), [&] {
                    int open = 0;
                    for (const UserSpan &u : row.userSpans)
                        open += u.startNs <= length / 3 && length / 3 < u.endNs;
                    return open;
                }());
                // Coarse (full range) and fine (zoomed, columns < length / 4096) bands.
                QCOMPARE(samplePhaseBand(*s, 0, 0, length, 700, level), refBand(*s, 0, 0, length, 700, level));
                const qint64 from = qint64(rng.bounded(quint64(length / 2)));
                QCOMPARE(samplePhaseBand(*s, 0, from, from + 20 * kMs, 400, level),
                         refBand(*s, 0, from, from + 20 * kMs, 400, level));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestTimelineViewMath)
#include "tst_trace_timeline_view_math.moc"
