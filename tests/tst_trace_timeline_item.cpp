#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

#include "otf2_fixture.h"
#include "otf2_trace_reader.h"
#include "timeline_builder.h"
#include "trace_aggregator.h"
#include "trace_overview_item.h"
#include "trace_timeline_item.h"
#include "trace_timeline_model.h"

using namespace trace;

namespace {

constexpr qint64 kMs = 1'000'000;
constexpr qint64 kAll = std::numeric_limits<qint64>::max();
constexpr int kL = TraceTimelineItem::kLabelWidth;
constexpr int kTop = TraceTimelineItem::kAxisHeight + TraceTimelineItem::kNodeHeader; // first band/bar

std::shared_ptr<TimelineStore> build(const QString &anchor, bool keep = true, int levels = 4)
{
    TraceAggregator agg;
    TimelineBuilder builder(agg, 0, kAll, keep, levels);
    TraceMeta meta;
    QString error;
    if (!readOtf2(anchor, builder, &meta, &error)) {
        qWarning("%s", qPrintable(error));
        return {};
    }
    agg.finish(meta.traceLengthNs);
    return builder.finish();
}

QImage render(TraceTimelineItem &item)
{
    QImage img(int(item.width()), int(item.height()), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    item.paint(&p);
    return img;
}

QImage render(TraceOverviewItem &item)
{
    QImage img(int(item.width()), int(item.height()), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    item.paint(&p);
    return img;
}

QColor px(const QImage &img, int x, int y)
{
    return QColor(img.pixel(x, y));
}

QString hex(const QColor &c)
{
    return c.name();
}

// Synthetic complete store: `ranks` ranks on 16 nodes, `perRank` activities each (period 100 us,
// 30 us long, cycling Send / Recv with 10 us wait / Collective / OtherMpi), a 2-level pyramid
// with a cycling pattern, main + 40 phases per rank and a ring message every 1000 activities.
std::shared_ptr<TimelineStore> syntheticStore(int ranks, int perRank)
{
    auto s = std::make_shared<TimelineStore>();
    const qint64 period = 100'000;
    const qint64 length = qint64(perRank) * period;
    s->traceLengthNs = length;
    s->fromNs = 0;
    s->toNs = length;
    s->complete = true;
    s->functions = {QStringLiteral("MPI_Send"), QStringLiteral("MPI_Recv"), QStringLiteral("MPI_Allreduce"),
                    QStringLiteral("MPI_Barrier"), QStringLiteral("main")};
    for (int k = 0; k < 40; ++k)
        s->functions.append(QStringLiteral("phase_%1").arg(k));
    const Category cats[4] = {Category::Send, Category::Recv, Category::Collective, Category::OtherMpi};
    s->rows.resize(ranks);
    for (int r = 0; r < ranks; ++r) {
        RankRow &row = s->rows[r];
        row.rank = r;
        row.node = QStringLiteral("node%1").arg(r / (ranks / 16 > 0 ? ranks / 16 : 1));
        row.activities.reserve(perRank);
        for (int i = 0; i < perRank; ++i) {
            const qint64 t = i * period;
            const int c = i % 4;
            row.activities.append({t, t + 30'000, c == 1 ? 10'000 : 0, 0, 0, quint16(c), cats[c]});
        }
        row.userSpans.append({0, length, 4, 0});
        const qint64 phaseLen = length / 40;
        for (int k = 0; k < 40; ++k)
            row.userSpans.append({k * phaseLen, (k + 1) * phaseLen, quint16(5 + k), 1});
        buildSpanIndex(row, length);
    }
    for (int i = 0; i < perRank; i += 1000) {
        for (int r = 0; r < ranks; ++r) {
            const int to = (r + 1) % ranks;
            const qint64 t = i * period;
            s->rows[r].messages.append(s->messages.size());
            s->rows[to].messages.append(s->messages.size());
            s->messages.append({r, to, t, t + 50'000, 64, 0, 0, 0, 0, 0, 1, to});
        }
    }
    Pyramid &p = s->pyramid;
    p.levels = 2;
    p.binsPerLevel = {1024, 4096};
    p.traceLengthNs = length;
    p.data.resize(2);
    for (int k = 0; k < 2; ++k) {
        p.data[k].resize(ranks);
        for (int r = 0; r < ranks; ++r) {
            QVector<quint16> &d = p.data[k][r];
            d.fill(0, p.binsPerLevel[k] * kCategoryCount);
            for (int b = 0; b < p.binsPerLevel[k]; ++b)
                d[b * kCategoryCount + 1 + (b + r) % 5] = 30000;
        }
    }
    return s;
}

} // namespace

class TstTraceTimelineItem : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_ring4;
    QString m_phases;
    QString m_many;

    // ring4 in full mode, showPhases off: 48 + 2500 px wide, view [0, 2.5 s] -> 1 px = 1 ms.
    void setupRing4(TraceTimelineModel &model, TraceTimelineItem &item)
    {
        model.addSnapshot({build(m_ring4)});
        item.setShowPhases(false);
        item.setModel(&model);
        item.setWidth(kL + 2500);
        item.setHeight(300);
        item.setViewStartNs(0);
        item.setViewEndNs(2500 * kMs);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QDir(m_dir.path()).mkpath(QStringLiteral("ring4"));
        QDir(m_dir.path()).mkpath(QStringLiteral("phases"));
        QDir(m_dir.path()).mkpath(QStringLiteral("many"));
        m_ring4 = writeRing4Otf2(m_dir.filePath(QStringLiteral("ring4")));
        m_phases = writePhasesOtf2(m_dir.filePath(QStringLiteral("phases")));
        m_many = writeManyMessagesOtf2(m_dir.filePath(QStringLiteral("many")), 4, 1000);
        QVERIFY(!m_ring4.isEmpty() && !m_phases.isEmpty() && !m_many.isEmpty());
    }

    void rendersPhaseBand()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_phases)});
        TraceTimelineItem item;
        item.setModel(&model);
        item.setWidth(kL + 3000);
        item.setHeight(200);
        item.setViewStartNs(0);
        item.setViewEndNs(3000 * kMs);
        QVERIFY(item.showPhases());
        const int bandY = kTop + TraceTimelineItem::kPhaseBand / 2;
        QImage img = render(item);
        const QColor a = px(img, kL + 300, bandY);
        const QColor b = px(img, kL + 2000, bandY);
        QVERIFY2(a != b, qPrintable(hex(a) + " vs " + hex(b)));
        QVERIFY2(a != QColor(Qt::white), "phase_a must be drawn");

        item.setPhaseLevel(QStringLiteral("inner"));
        QCOMPARE(item.phaseLevel(), QStringLiteral("inner"));
        img = render(item);
        const QColor inner = px(img, kL + 550, bandY);
        const QColor outer = px(img, kL + 300, bandY);
        QVERIFY2(inner != outer, qPrintable(hex(inner) + " vs " + hex(outer)));
    }

    void rendersRing4Colours()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        const QImage img = render(item);
        const int r0 = kTop + 9;
        const int r1 = kTop + TraceTimelineItem::kBarRow + 9;
        const QString wait = hex(px(img, kL + 250, r0));
        QVERIFY2(wait == QStringLiteral("#e0b84a") || wait == QStringLiteral("#a07a10"), qPrintable(wait));
        QCOMPARE(hex(px(img, kL + 100, r0)), QStringLiteral("#4caf50"));
        QCOMPARE(hex(px(img, kL + 1100, r0)), QStringLiteral("#3b82f6"));
        QCOMPARE(hex(px(img, kL + 250, r1)), QStringLiteral("#c9ced3"));
    }

    void contentHeightFollowsLayout()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        // 2 nodes, 4 ranks, no phase band.
        QCOMPARE(item.contentHeight(), qreal(2 * 16 + 4 * 18));
        item.setShowPhases(true);
        QCOMPARE(item.contentHeight(), qreal(2 * 16 + 4 * 30));
    }

    void zoomClampsToMinimumWindow()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        for (int i = 0; i < 20; ++i)
            item.zoomAt(kL + 1000, 0.1);
        QCOMPARE(item.viewEndNs() - item.viewStartNs(), qint64(10'000));
        QVERIFY(item.viewStartNs() >= 0);
        // The time under x stays fixed (1 s at x = 48 + 1000).
        QVERIFY(qAbs(item.viewStartNs() + 0.4 * 10'000 - 1000.0 * kMs) < 2);
        for (int i = 0; i < 20; ++i)
            item.zoomAt(kL + 1000, 10);
        QCOMPARE(item.viewStartNs(), qint64(0));
        QCOMPARE(item.viewEndNs(), model.traceLengthNs());
    }

    void panClampsToTraceBounds()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        item.zoomAt(kL + 1250, 0.1);
        const qint64 span = item.viewEndNs() - item.viewStartNs();
        item.panBy(-1e6);
        QCOMPARE(item.viewStartNs(), qint64(0));
        QCOMPARE(item.viewEndNs() - item.viewStartNs(), span);
        item.panBy(+1e6);
        QCOMPARE(item.viewEndNs(), model.traceLengthNs());
        QCOMPARE(item.viewEndNs() - item.viewStartNs(), span);
        item.showFullRun();
        QCOMPARE(item.viewStartNs(), qint64(0));
        QCOMPARE(item.viewEndNs(), model.traceLengthNs());
    }

    void setViewSetsBothEndsClamped()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        QSignalSpy spy(&item, &TraceTimelineItem::viewChanged);
        item.setView(1000 * kMs, 1250 * kMs);
        QCOMPARE(spy.count(), 1); // one change, no intermediate range
        QCOMPARE(item.viewStartNs(), 1000 * kMs);
        QCOMPARE(item.viewEndNs(), 1250 * kMs);
        item.setView(-500 * kMs, -400 * kMs); // before the trace: moved in, span kept
        QCOMPARE(item.viewStartNs(), qint64(0));
        QCOMPARE(item.viewEndNs(), 100 * kMs);
        item.setView(2000 * kMs, 2000 * kMs); // empty: minimum window
        QCOMPARE(item.viewEndNs() - item.viewStartNs(), qint64(10'000));
        item.setView(5 * kMs, 1000 * 1000 * kMs); // longer than the trace: full run
        QCOMPARE(item.viewStartNs(), qint64(0));
        QCOMPARE(item.viewEndNs(), model.traceLengthNs());
    }

    void clickLabelSelectsRank()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        // R2 is the first rank of nodeB: axis + header A + 2 rows + header B.
        const qreal r2 = 22 + 16 + 2 * 18 + 16 + 9;
        item.clickAt(10, r2);
        QCOMPARE(item.selectedRank(), 2);
        item.clickAt(10, r2);
        QCOMPARE(item.selectedRank(), -1);
        item.clickAt(10, 5); // axis
        QCOMPARE(item.selectedRank(), -1);
    }

    void clickBarEmitsDetails()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        QSignalSpy spy(&item, &TraceTimelineItem::barClicked);
        item.clickAt(kL + 200, kTop + 9);
        QCOMPARE(spy.count(), 1);
        const QVariantMap d = spy.first().at(0).toMap();
        QCOMPARE(d.value(QStringLiteral("function")).toString(), QStringLiteral("MPI_Recv"));
        QCOMPARE(d.value(QStringLiteral("rank")).toInt(), 0);
        QCOMPARE(d.value(QStringLiteral("waitUs")).toDouble(), 299000.0);
        QCOMPARE(d.value(QStringLiteral("startUs")).toDouble(), 101000.0);
        QCOMPARE(d.value(QStringLiteral("durationUs")).toDouble(), 301000.0);
        QCOMPARE(d.value(QStringLiteral("partner")).toInt(), 3);
        for (const char *key : {"bytes", "tag", "comm", "callPath", "messageCount", "completes"})
            QVERIFY2(d.contains(QLatin1String(key)), key);
        item.clickAt(kL + 200, 5); // axis: nothing
        QCOMPARE(spy.count(), 1);
    }

    void dimsRowsWithoutMessagesToSelectedRank()
    {
        TraceTimelineModel model;
        TraceTimelineItem item;
        setupRing4(model, item);
        item.setViewStartNs(0);
        item.setViewEndNs(999 * kMs); // second 0: R1 exchanges with R0 and R2 only
        item.setSelectedRank(1);
        const QImage img = render(item);
        const int compX = kL + 2000; // compute in all rows
        const qreal r3 = 22 + 16 + 2 * 18 + 16 + 18 + 9;
        const QColor full = px(img, compX, kTop + 9);     // R0: partner
        const QColor dim = px(img, compX, int(r3));       // R3: no message with R1
        QCOMPARE(hex(full), QStringLiteral("#c9ced3"));
        QVERIFY2(dim.lightness() > full.lightness(), qPrintable(hex(dim)));
    }

    void arrowsAreCapped()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_many)});
        TraceTimelineItem item;
        item.setModel(&model);
        item.setWidth(1500);
        item.setHeight(400);
        item.showFullRun();
        QVERIFY(!item.arrowsCapped());
        QSignalSpy spy(&item, &TraceTimelineItem::arrowsCappedChanged);
        item.setAllArrows(true);
        render(item);
        QVERIFY(item.arrowsCapped());
        QCOMPARE(spy.count(), 1);
        item.setAllArrows(false);
        QVERIFY(!item.arrowsCapped());
    }

    void requestsDetailOnlyWhenViewChanges()
    {
        TraceTimelineModel model;
        auto overview = build(m_ring4, false);
        QVERIFY(overview && !overview->complete);
        model.addSnapshot({overview, -1, -1, 1000 * kMs});
        QVERIFY(model.windowed());
        QSignalSpy requests(&model, &TraceTimelineModel::detailRequested);
        QSignalSpy changed(&model, &TraceTimelineModel::changed);
        TraceTimelineItem item;
        item.setModel(&model);
        item.setWidth(kL + 2500);
        item.setHeight(300);
        item.showFullRun(); // wider than maxDetailWindowNs: overview only
        render(item);
        QTest::qWait(TraceTimelineModel::kDebounceMs + 100);
        QCOMPARE(requests.count(), 0);
        QCOMPARE(changed.count(), 0);

        item.zoomAt(kL + 1250, 0.1); // 250 ms window
        const int changedAfterZoom = changed.count();
        QCOMPARE(changedAfterZoom, 1); // one accepted requestDetail
        for (int i = 0; i < 5; ++i)
            render(item); // repaints never request
        QCOMPARE(changed.count(), changedAfterZoom);
        QTRY_COMPARE(requests.count(), 1);
        QCOMPARE(requests.first().at(0).toLongLong(), item.viewStartNs());
        QCOMPARE(requests.first().at(1).toLongLong(), item.viewEndNs());

        // Answer with a window covering the view: repaint draws it, nothing further requested.
        TraceAggregator agg;
        TimelineBuilder builder(agg, item.viewStartNs(), item.viewEndNs(), true, 0);
        TraceMeta meta;
        QString error;
        QVERIFY(readOtf2(m_ring4, builder, &meta, &error));
        agg.finish(meta.traceLengthNs);
        model.addSnapshot({builder.finish(), item.viewStartNs(), item.viewEndNs()});
        QVERIFY(model.detail());
        item.setWidth(kL + 2000); // resize, same range: covered, no request
        render(item);
        QTest::qWait(TraceTimelineModel::kDebounceMs + 100);
        QCOMPARE(requests.count(), 1);
    }

    // A hidden timeline (e.g. a page a StackView keeps below the visible one) does not request
    // detail on resize or view changes; it requests once when it becomes visible.
    void hiddenItemRequestsDetailOnlyWhenShown()
    {
        TraceTimelineModel model;
        auto overview = build(m_ring4, false);
        QVERIFY(overview && !overview->complete);
        model.addSnapshot({overview, -1, -1, 1000 * kMs});
        QVERIFY(model.windowed());
        QSignalSpy requests(&model, &TraceTimelineModel::detailRequested);
        // root > page (hidden like a StackView page below the top) > item. (A parentless item
        // cannot become visible again: Qt derives effective visibility from the parent.)
        QQuickItem root;
        QQuickItem page(&root);
        page.setParentItem(&root);
        TraceTimelineItem item(&page);
        item.setParentItem(&page);
        QVERIFY(item.isVisible());
        page.setVisible(false);
        QVERIFY(!item.isVisible());
        item.setModel(&model);
        item.setWidth(kL + 2500);
        item.setHeight(300);
        item.setView(0, 250 * kMs);
        item.setWidth(kL + 2000); // resize
        item.zoomAt(kL + 100, 0.5);
        item.panBy(50);
        QTest::qWait(TraceTimelineModel::kDebounceMs + 100);
        QCOMPARE(requests.count(), 0);
        QVERIFY(!model.loadingDetail());

        const qint64 start = item.viewStartNs();
        const qint64 end = item.viewEndNs();
        page.setVisible(true);
        QVERIFY(item.isVisible());
        QTRY_COMPARE(requests.count(), 1);
        QCOMPARE(requests.first().at(0).toLongLong(), start);
        QCOMPARE(requests.first().at(1).toLongLong(), end);
        QTest::qWait(TraceTimelineModel::kDebounceMs + 100);
        QCOMPARE(requests.count(), 1); // once
    }

    void overviewDrawsWindowAndMovesIt()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_ring4)});
        TraceOverviewItem item;
        item.setModel(&model);
        item.setWidth(1000);
        item.setHeight(40);
        item.setViewStartNs(1000 * kMs);
        item.setViewEndNs(1250 * kMs);
        const QImage img = render(item);
        // Collective second (all ranks in MPI_Allreduce [1000, 1201] mostly) is blue at 1.1 s.
        QCOMPARE(hex(px(img, 440, 20)), QStringLiteral("#3b82f6"));
        // Compute at 1.8 s.
        QCOMPARE(hex(px(img, 720, 20)), QStringLiteral("#c9ced3"));

        QSignalSpy spy(&item, &TraceOverviewItem::windowMoved);
        item.moveWindowTo(0); // centre at 0 -> clamped to start
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toLongLong(), qint64(0));
        QCOMPARE(spy.last().at(1).toLongLong(), 250 * kMs);
        item.moveWindowTo(1000); // clamped to the end
        QCOMPARE(spy.last().at(0).toLongLong(), 2250 * kMs);
        QCOMPARE(spy.last().at(1).toLongLong(), 2500 * kMs);
        item.moveWindowTo(500);
        QCOMPARE(spy.last().at(0).toLongLong(), 1125 * kMs);
    }

    void renderPerformanceSmoke()
    {
        TraceTimelineModel model;
        model.addSnapshot({syntheticStore(256, 40'000)});
        TraceTimelineItem item;
        item.setModel(&model);
        item.setWidth(1500);
        item.setHeight(900);
        item.setSelectedRank(0);
        item.showFullRun();
        QElapsedTimer t;
        render(item); // warm-up (fonts)
        t.start();
        render(item);
        const qint64 fullMs = t.elapsed();
        item.setViewStartNs(2000 * kMs);
        item.setViewEndNs(2050 * kMs);
        t.restart();
        render(item);
        const qint64 windowMs = t.elapsed();
        item.setContentY(item.contentHeight() / 2);
        t.restart();
        render(item);
        const qint64 scrolledMs = t.elapsed();
        qInfo("paint 256 ranks x 40000: full view %lld ms, 50 ms window %lld ms, scrolled %lld ms", fullMs,
              windowMs, scrolledMs);
        QVERIFY2(fullMs < 500, "full view paint must stay bounded");
        QVERIFY2(windowMs < 500, "50 ms window paint must stay bounded");
        QVERIFY2(scrolledMs < 500, "scrolled paint must stay bounded");
    }
};

QTEST_MAIN(TstTraceTimelineItem)
#include "tst_trace_timeline_item.moc"
