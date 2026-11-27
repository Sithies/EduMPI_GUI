#include <QElapsedTimer>
#include <QImage>
#include <QJSValue>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <limits>

#include "otf2_fixture.h"
#include "otf2_trace_reader.h"
#include "timeline_builder.h"
#include "trace_aggregator.h"
#include "trace_graph_item.h"
#include "trace_timeline_item.h"
#include "trace_timeline_model.h"

using namespace trace;

namespace {

constexpr qint64 kMs = 1'000'000;
constexpr qint64 kAll = std::numeric_limits<qint64>::max();

std::shared_ptr<TimelineStore> build(const QString &anchor, bool keep = true, qint64 from = 0, qint64 to = kAll)
{
    TraceAggregator agg;
    TimelineBuilder builder(agg, from, to, keep, keep ? 2 : 4);
    TraceMeta meta;
    QString error;
    if (!readOtf2(anchor, builder, &meta, &error)) {
        qWarning("%s", qPrintable(error));
        return {};
    }
    agg.finish(meta.traceLengthNs);
    return builder.finish();
}

QImage render(TraceGraphItem &item)
{
    QImage img(int(item.width()), int(item.height()), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    item.paint(&p);
    return img;
}

int nonWhite(const QImage &img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (QColor(img.pixel(x, y)) != QColor(Qt::white))
                ++n;
    return n;
}

int edgeIndex(const CommGraph &g, int fromRank, int toRank)
{
    for (int i = 0; i < g.edges.size(); ++i)
        if (g.edges[i].from == g.indexOfRank(fromRank) && g.edges[i].to == g.indexOfRank(toRank))
            return i;
    return -1;
}

} // namespace

class TstTraceGraphItem : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_ring4;

    // ring4 in full mode, 600 x 400, window = second 0 (the ring).
    void setupRing4(TraceTimelineModel &model, TraceGraphItem &item)
    {
        model.addSnapshot({build(m_ring4)});
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        item.setModel(&model);
        item.setWidth(600);
        item.setHeight(400);
        item.setViewStartNs(0);
        item.setViewEndNs(1000 * kMs);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_ring4 = writeRing4Otf2(m_dir.filePath(QStringLiteral("r4")));
        QVERIFY(!m_ring4.isEmpty());
    }

    void drawsRingOfFirstSecond()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        QCOMPARE(item.status(), QString());
        QCOMPARE(item.edgeCount(), 4);
        QCOMPARE(item.shownEdgeCount(), 4);
        QVERIFY(std::abs(item.maxWaitSeconds() - 0.299) < 1e-6);
        const QImage img = render(item);
        QCOMPARE(img.pixelColor(0, 0), QColor(Qt::white)); // background
        // Rank 0 sits at the top of the circle. It is the only rank that waits (3 -> 0, late
        // sender), so it is filled with the blue end of the scale (as in the 3D view), rank 1 white.
        const QPointF p0 = item.nodePosition(item.graph().indexOfRank(0));
        QVERIFY(std::abs(p0.x() - 300) < 1e-6);
        QCOMPARE(QColor(img.pixel(int(p0.x()), int(p0.y()))).name(), QStringLiteral("#0000ff"));
        QVERIFY(std::abs(item.nodeWaitSeconds(item.graph().indexOfRank(0)) - 0.299) < 1e-6);
        const QPointF p1 = item.nodePosition(item.graph().indexOfRank(1));
        QCOMPARE(QColor(img.pixel(int(p1.x()), int(p1.y()))).name(), QStringLiteral("#ffffff"));
        // Line colour = waiting on the link: 3 -> 0 (late sender, the most waiting) red, 0 -> 1
        // (no waiting) green.
        {
            const QPointF mid = item.edgeMidpoint(edgeIndex(item.graph(), 3, 0));
            const QColor c(img.pixel(int(mid.x()), int(mid.y())));
            QVERIFY2(c.red() > c.green() + 60 && c.red() > c.blue() + 60, qPrintable(c.name()));
            const QPointF mid1 = item.edgeMidpoint(edgeIndex(item.graph(), 0, 1));
            const QColor c1(img.pixel(int(mid1.x()), int(mid1.y())));
            QVERIFY2(c1.green() > c1.red() + 40 && c1.green() > c1.blue() + 40, qPrintable(c1.name()));
        }
        // Legend of the line colour and other colour modes.
        QCOMPARE(item.edgeLegend().value(QStringLiteral("colors")).toStringList().size(), 3);
        QVERIFY(item.edgeLegend().value(QStringLiteral("high")).toString().contains(QStringLiteral("ms")));
        const int e30 = edgeIndex(item.graph(), 3, 0);
        item.setEdgeColor(QStringLiteral("locality"));
        QVERIFY(item.edgeLegend().contains(QStringLiteral("categories")));
        // ring4: ranks 0, 1 on nodeA, 2, 3 on nodeB: 3 -> 0 crosses nodes.
        QCOMPARE(item.edgeColorOf(e30).name(), QStringLiteral("#1565c0"));
        QCOMPARE(item.edgeColorOf(edgeIndex(item.graph(), 0, 1)).name(), QStringLiteral("#66bb6a"));
        item.setEdgeColor(QStringLiteral("volume"));
        QVERIFY(item.edgeLegend().value(QStringLiteral("title")).toString().contains(QStringLiteral("bytes")));
        item.setEdgeColor(QStringLiteral("nonsense"));
        QCOMPARE(item.edgeColor(), QStringLiteral("wait"));
        // Node fill by compute time: legend switches.
        item.setNodeColor(QStringLiteral("compute"));
        QVERIFY(item.nodeLegend().value(QStringLiteral("title")).toString().contains(QStringLiteral("compute")));
        QVERIFY(!render(item).isNull());
        item.setNodeColor(QStringLiteral("wait"));
        // Edge filter: only between nodes / only with waiting.
        item.setEdgeFilter(QStringLiteral("between"));
        for (int i : item.drawnEdges()) {
            const auto &e = item.graph().edges[i];
            QVERIFY(e.kind == GraphEdge::Kind::P2P);
            QVERIFY(item.graph().nodes[e.from].group != item.graph().nodes[e.to].group);
        }
        item.setEdgeFilter(QStringLiteral("waiting"));
        for (int i : item.drawnEdges())
            QVERIFY(item.graph().edges[i].waitSeconds > 0);
        QVERIFY(item.drawnEdges().contains(e30));
        item.setEdgeFilter(QStringLiteral("all"));
        // Zoom to a rank selects it.
        QVERIFY(item.zoomToRank(2));
        QCOMPARE(item.selectedRank(), 2);
        QVERIFY(!item.zoomToRank(77));
        item.resetView();
    }

    void wholeRunWhenWindowEmpty()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_ring4)});
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        item.setModel(&model);
        item.setWidth(600);
        item.setHeight(400);
        // Ring (4) + allreduce star (4) + second 2 (1 -> 2 adds to the ring's edge, 3 -> 2).
        QCOMPARE(item.edgeCount(), 9);
        QCOMPARE(item.graph().nodes.size(), 5);
    }

    void optionsRebuild()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        QSignalSpy changed(&item, &TraceGraphItem::graphChanged);
        item.setDirected(false);
        QCOMPARE(item.edgeCount(), 4); // ring: no pair in both directions
        item.setViewEndNs(0);          // whole run
        item.setLevel(QStringLiteral("nodes"));
        QCOMPARE(item.graph().rankNodeCount, 2);
        QCOMPARE(item.level(), QStringLiteral("nodes"));
        item.setShowCollectives(false);
        item.setShowP2P(false);
        QCOMPARE(item.edgeCount(), 0);
        QVERIFY(changed.count() > 0);
        QVERIFY(nonWhite(render(item)) > 0); // nodes and the "no communication" hint
        item.setLayout(QStringLiteral("grid"));
        QCOMPARE(item.layout(), QStringLiteral("grid"));
        item.setWeight(QStringLiteral("wait"));
        QCOMPARE(item.weight(), QStringLiteral("wait"));
        item.setWeight(QStringLiteral("nonsense"));
        QCOMPARE(item.weight(), QStringLiteral("bytes"));
    }

    void maxEdgesKeepsSelectedRank()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        item.setMaxEdges(1);
        QCOMPARE(item.shownEdgeCount(), 1);
        // All ring edges weigh 1000 B: the first one (0 -> 1) is kept, rank 2's two join it.
        item.setSelectedRank(2);
        QCOMPARE(item.shownEdgeCount(), 3);
        item.setMaxEdges(0);
        QCOMPARE(item.shownEdgeCount(), 4);
    }

    void clickSelectsRankAndEdge()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        QSignalSpy rank(&item, &TraceGraphItem::selectedRankChanged);
        const QPointF p2 = item.nodePosition(item.graph().indexOfRank(2));
        item.clickAt(p2.x(), p2.y());
        QCOMPARE(item.selectedRank(), 2);
        item.clickAt(p2.x(), p2.y());
        QCOMPARE(item.selectedRank(), -1);
        QCOMPARE(rank.count(), 2);

        QSignalSpy edge(&item, &TraceGraphItem::edgeClicked);
        const QPointF mid = item.edgeMidpoint(edgeIndex(item.graph(), 3, 0));
        item.clickAt(mid.x(), mid.y());
        QCOMPARE(edge.count(), 1);
        const QVariantMap d = edge.first().first().toMap();
        QCOMPARE(d.value(QStringLiteral("kind")).toString(), QStringLiteral("p2p"));
        QCOMPARE(d.value(QStringLiteral("fromRank")).toInt(), 3);
        QCOMPARE(d.value(QStringLiteral("toRank")).toInt(), 0);
        QCOMPARE(d.value(QStringLiteral("bytes")).toULongLong(), 1000ULL);
        QCOMPARE(d.value(QStringLiteral("messages")).toInt(), 1);
        QVERIFY(std::abs(d.value(QStringLiteral("waitSeconds")).toDouble() - 0.299) < 1e-6);
        QCOMPARE(d.value(QStringLiteral("toNs")).toLongLong(), 1000 * kMs);

        // Empty space: nothing.
        item.clickAt(2, 2);
        QCOMPARE(edge.count(), 1);
        QCOMPARE(item.selectedRank(), -1);
    }

    void collectiveEdgeDetails()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        item.setViewStartNs(1000 * kMs);
        item.setViewEndNs(2000 * kMs);
        QCOMPARE(item.edgeCount(), 4);
        const int comm = item.graph().rankNodeCount;
        QCOMPARE(item.nodePosition(comm), QPointF(300, 200)); // centre of the circle
        int e = -1;
        for (int i = 0; i < item.graph().edges.size(); ++i)
            if (item.graph().edges[i].from == item.graph().indexOfRank(1))
                e = i;
        QVERIFY(e >= 0);
        QSignalSpy edge(&item, &TraceGraphItem::edgeClicked);
        const QPointF mid = item.edgeMidpoint(e);
        item.clickAt(mid.x(), mid.y());
        QCOMPARE(edge.count(), 1);
        const QVariantMap d = edge.first().first().toMap();
        QCOMPARE(d.value(QStringLiteral("kind")).toString(), QStringLiteral("collective"));
        QCOMPARE(d.value(QStringLiteral("fromRank")).toInt(), 1);
        QCOMPARE(d.value(QStringLiteral("toRank")).toInt(), -1);
        QVERIFY(d.value(QStringLiteral("comm")).isValid());
    }

    // Windowed trace: a window longer than maxDetailWindowNs is not loaded ("zoom"), a shorter
    // one is requested (as the timeline does) and drawn once the answer covers it.
    void windowedLoadsDetailOnDemand()
    {
        TraceTimelineModel model;
        auto overview = build(m_ring4, false);
        QVERIFY(overview && !overview->complete);
        model.addSnapshot({overview, -1, -1, 1000 * kMs});
        QSignalSpy requests(&model, &TraceTimelineModel::detailRequested);
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        item.setModel(&model);
        item.setWidth(600);
        item.setHeight(400);
        QCOMPARE(item.status(), QStringLiteral("zoom")); // whole run, 2.5 s
        QCOMPARE(item.graph().rankNodeCount, 4);         // nodes are placed anyway
        QCOMPARE(item.edgeCount(), 0);

        item.setViewStartNs(0);
        item.setViewEndNs(1000 * kMs);
        QCOMPARE(item.status(), QStringLiteral("loading"));
        QTRY_COMPARE(requests.count(), 1);
        QCOMPARE(requests.first().at(0).toLongLong(), 0LL);
        QCOMPARE(requests.first().at(1).toLongLong(), 1000 * kMs);

        model.addSnapshot({build(m_ring4, true, 0, 1000 * kMs), 0, 1000 * kMs});
        QCOMPARE(item.status(), QString());
        QCOMPARE(item.edgeCount(), 4);
    }

    void hiddenItemDoesNotRequest()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_ring4, false), -1, -1, 1000 * kMs});
        QSignalSpy requests(&model, &TraceTimelineModel::detailRequested);
        QQuickItem root;
        QQuickItem page(&root);
        page.setParentItem(&root);
        TraceGraphItem item(&page);
        item.setParentItem(&page);
        page.setVisible(false);
        item.setModel(&model);
        item.setViewStartNs(0);
        item.setViewEndNs(500 * kMs);
        QTest::qWait(TraceTimelineModel::kDebounceMs + 100);
        QCOMPARE(requests.count(), 0);
        page.setVisible(true);
        QTRY_COMPARE(requests.count(), 1);
    }

    void manyRanksStayFast()
    {
        QTemporaryDir tmp;
        const QString anchor = writeManyMessagesOtf2(tmp.filePath(QStringLiteral("many")), 256, 20);
        QVERIFY(!anchor.isEmpty());
        TraceTimelineModel model;
        model.addSnapshot({build(anchor)});
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        item.setWidth(900);
        item.setHeight(700);
        QElapsedTimer t;
        t.start();
        item.setModel(&model);
        const QImage img = render(item);
        QCOMPARE(item.graph().rankNodeCount, 256);
        QCOMPARE(item.edgeCount(), 256); // the ring
        QVERIFY(nonWhite(img) > 1000);
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
    }

    // Auto level: many ranks in a long window -> compute nodes; zoomed in -> ranks.
    void autoLevelFollowsZoom()
    {
        QTemporaryDir tmp;
        const QString anchor = writeManyMessagesOtf2(tmp.filePath(QStringLiteral("many")), 128, 20, 100 * kMs);
        QVERIFY(!anchor.isEmpty());
        TraceTimelineModel model;
        model.addSnapshot({build(anchor)});
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        item.setWidth(600);
        item.setHeight(400);
        item.setModel(&model);
        QCOMPARE(item.level(), QStringLiteral("auto"));
        QCOMPARE(item.effectiveLevel(), QStringLiteral("nodes")); // whole run, > 100 ms
        QCOMPARE(item.graph().rankNodeCount, 2);
        item.setViewStartNs(0);
        item.setViewEndNs(3 * kMs);
        QCOMPARE(item.effectiveLevel(), QStringLiteral("ranks"));
        QCOMPARE(item.graph().rankNodeCount, 128);
        // Fixed levels ignore the zoom.
        item.setLevel(QStringLiteral("nodes"));
        QCOMPARE(item.effectiveLevel(), QStringLiteral("nodes"));
        item.setLevel(QStringLiteral("ranks"));
        item.setViewEndNs(0); // whole run
        QCOMPARE(item.effectiveLevel(), QStringLiteral("ranks"));
    }

    // Nodes level: double-click opens a compute node into its ranks and closes it again;
    // tooltips explain nodes and edges; traffic inside a node draws a green ring.
    void nodesExpandAndExplain()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item); // ranks 0, 1 on nodeA; 2, 3 on nodeB; ring 0->1->2->3->0
        item.setLevel(QStringLiteral("nodes"));
        QCOMPARE(item.graph().rankNodeCount, 2);
        QCOMPARE(item.graph().nodes[0].internalMessages, 1); // 0 -> 1 inside nodeA
        const QPointF a = item.nodePosition(0);
        const QString tip = item.toolTipAt(a.x(), a.y());
        QVERIFY2(tip.contains(QStringLiteral("Compute node nodeA with 2 ranks")), qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("Inside the node")), qPrintable(tip));
        // Green ring just outside the node.
        const QImage img = render(item);
        bool green = false;
        for (int dy = 9; dy <= 16 && !green; ++dy) {
            const QColor c(img.pixel(int(a.x()), int(a.y()) + dy));
            green = c.green() > 150 && c.red() < 120 && c.blue() < 120;
        }
        QVERIFY(green);
        // Open nodeA: its two ranks + nodeB.
        QVERIFY(item.toggleExpandAt(a.x(), a.y()));
        QCOMPARE(item.expandedCount(), 1);
        QCOMPARE(item.graph().rankNodeCount, 3);
        const int r0 = item.graph().indexOfRank(0);
        QVERIFY(r0 >= 0);
        QVERIFY(edgeIndex(item.graph(), 0, 1) >= 0); // now a drawn edge between the ranks
        const QPointF p0 = item.nodePosition(r0);
        QVERIFY(item.toolTipAt(p0.x(), p0.y()).contains(QStringLiteral("Rank 0 (compute node nodeA)")));
        // Double-click on one of its ranks merges it again.
        QVERIFY(item.toggleExpandAt(p0.x(), p0.y()));
        QCOMPARE(item.expandedCount(), 0);
        QCOMPARE(item.graph().rankNodeCount, 2);
        // Ranks level: no expanding; edge tooltip.
        item.setLevel(QStringLiteral("ranks"));
        const QPointF pr = item.nodePosition(item.graph().indexOfRank(0));
        QVERIFY(!item.toggleExpandAt(pr.x(), pr.y()));
        const QPointF mid = item.edgeMidpoint(edgeIndex(item.graph(), 3, 0));
        const QString et = item.toolTipAt(mid.x(), mid.y());
        QVERIFY2(et.startsWith(QStringLiteral("Messages: rank 3 → rank 0")), qPrintable(et));
        QVERIFY2(et.contains(QStringLiteral("299.000 ms")), qPrintable(et));
        QCOMPARE(item.toolTipAt(1, 1), QString());
    }

    void communicatorsMergeOnDemand()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        item.setViewEndNs(0); // whole run: one communicator
        QCOMPARE(item.communicators(), QStringLiteral("auto"));
        QVERIFY(!item.communicatorsMerged()); // a single one is never merged
        item.setCommunicators(QStringLiteral("merged"));
        QVERIFY(!item.communicatorsMerged());
        QCOMPARE(item.graph().nodes.size(), 5);
    }

    // Many ranks: data-driven layout (synchronous here), zoom / pan, findings and highlighting.
    // All links drawn (up to the limit), the heaviest kNotableEdges strong, the rest pale;
    // the summary sentence over the findings.
    void allEdgesDrawnHeaviestStrong()
    {
        QTemporaryDir tmp;
        const int n = 24; // all-to-all: 552 links
        QVector<QPair<int, int>> edges;
        for (int a = 0; a < n; ++a)
            for (int b = 0; b < n; ++b)
                if (a != b)
                    edges << QPair<int, int>{a, b};
        const QString anchor = writeEdgesOtf2(tmp.path(), n, edges, 1);
        QVERIFY(!anchor.isEmpty());
        TraceTimelineModel model;
        model.addSnapshot({build(anchor)});
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setModel(&model);
        item.setWidth(800);
        item.setHeight(600);
        item.setLevel(QStringLiteral("ranks"));
        item.setShowCollectives(false);
        QCOMPARE(item.maxEdges(), TraceGraphItem::kDefaultMaxEdges);
        QCOMPARE(item.edgeCount(), n * (n - 1));
        QCOMPARE(item.shownEdgeCount(), n * (n - 1));
        QCOMPARE(item.notableEdgeCount(), TraceGraphItem::kNotableEdges);
        QVERIFY(nonWhite(render(item)) > 0);
        item.setMaxEdges(100);
        QCOMPARE(item.shownEdgeCount(), 100);
        QCOMPARE(item.notableEdgeCount(), 100);
        // Summary: one sentence with the waiting share.
        const QVariantMap sum = item.summary();
        QVERIFY2(sum.value(QStringLiteral("text")).toString().contains(QStringLiteral("%")),
                 qPrintable(sum.value(QStringLiteral("text")).toString()));
        QVERIFY(sum.value(QStringLiteral("finding")).toInt() < item.findingMaps().size());
    }

    void manyRanksLayoutZoomAndFindings()
    {
        QTemporaryDir tmp;
        // 8 x 6 process grid (not periodic), so Auto lays the ranks out as a grid.
        QVector<QPair<int, int>> edges;
        const int cols = 8, n = 48;
        for (int r = 0; r < n; ++r) {
            if (r % cols + 1 < cols)
                edges << QPair<int, int>{r, r + 1} << QPair<int, int>{r + 1, r};
            if (r + cols < n)
                edges << QPair<int, int>{r, r + cols} << QPair<int, int>{r + cols, r};
        }
        const QString anchor = writeEdgesOtf2(tmp.path(), n, edges, 2);
        QVERIFY(!anchor.isEmpty());
        TraceTimelineModel model;
        model.addSnapshot({build(anchor)});
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setModel(&model);
        item.setWidth(900);
        item.setHeight(700);
        item.setLevel(QStringLiteral("ranks"));
        item.setShowCollectives(false);
        QCOMPARE(item.status(), QString());
        QCOMPARE(item.layout(), QStringLiteral("auto"));
        QCOMPARE(item.effectiveLayout(), QStringLiteral("stress"));
        // Grid neighbours next to each other in the picture: 1 and 9 are nearer to 0 than 2 or 16.
        auto dist = [&](int a, int b) {
            const QPointF d = item.nodePosition(item.graph().indexOfRank(a)) - item.nodePosition(item.graph().indexOfRank(b));
            return std::hypot(d.x(), d.y());
        };
        QVERIFY(dist(0, 1) < dist(0, 2));
        QVERIFY(dist(0, 8) < dist(0, 16));
        QVERIFY(dist(0, 1) < dist(0, 47) / 4);
        QVERIFY(nonWhite(render(item)) > 0);

        // Zoom around a point keeps that point; pan moves; reset.
        const QPointF p0 = item.nodePosition(item.graph().indexOfRank(0));
        item.zoomAt(p0.x(), p0.y(), 3);
        QVERIFY(std::abs(item.zoom() - 3) < 1e-9);
        const QPointF p0z = item.nodePosition(item.graph().indexOfRank(0));
        QVERIFY(std::hypot(p0z.x() - p0.x(), p0z.y() - p0.y()) < 1e-6);
        QVERIFY(dist(0, 1) > 0);
        item.panBy(10, -5);
        const QPointF p0p = item.nodePosition(item.graph().indexOfRank(0));
        QVERIFY(std::abs(p0p.x() - p0z.x() - 10) < 1e-6 && std::abs(p0p.y() - p0z.y() + 5) < 1e-6);
        item.zoomAt(0, 0, 0.001); // clamped to 1
        QCOMPARE(item.zoom(), 1.0);
        item.resetView();
        QCOMPARE(item.pan(), QPointF());

        // Findings as maps; highlighting a finding lights its ranks; zoom to it.
        QVERIFY(!item.findingMaps().isEmpty());
        const QVariantMap pattern = item.findingMaps().first().toMap();
        QCOMPARE(pattern.value(QStringLiteral("kind")).toString(), QStringLiteral("pattern"));
        QVERIFY2(pattern.value(QStringLiteral("metric")).toString().contains(QStringLiteral("8 × 6")),
                 qPrintable(pattern.value(QStringLiteral("metric")).toString()));
        // A finding with ranks: pick one (outlier / late sender / link) if present.
        int withRanks = -1;
        for (int i = 0; i < item.findingMaps().size(); ++i)
            if (!item.findingMaps()[i].toMap().value(QStringLiteral("ranks")).toList().isEmpty())
                withRanks = i;
        if (withRanks >= 0) {
            QSignalSpy changed(&item, &TraceGraphItem::highlightChanged);
            item.zoomToFinding(withRanks);
            QCOMPARE(item.highlightedFinding(), withRanks);
            QCOMPARE(changed.count(), 1);
            const int r = item.findingMaps()[withRanks].toMap().value(QStringLiteral("ranks")).toList().first().toInt();
            QVERIFY(item.highlightedNodes().value(item.graph().indexOfRank(r)));
            QVERIFY(item.zoom() >= 1);
            QVERIFY(nonWhite(render(item)) > 0);
            item.setHighlightedFinding(-1);
            QVERIFY(!item.highlightedNodes().contains(true));
        }

        // Fixed layouts.
        item.setLayout(QStringLiteral("circle"));
        QCOMPARE(item.effectiveLayout(), QStringLiteral("circle"));
        item.setLayout(QStringLiteral("grid"));
        QCOMPARE(item.effectiveLayout(), QStringLiteral("grid"));
        // The detected 8-wide grid: rank 9 right below rank 1.
        const QPointF g1 = item.nodePosition(item.graph().indexOfRank(1));
        const QPointF g9 = item.nodePosition(item.graph().indexOfRank(9));
        QVERIFY(std::abs(g1.x() - g9.x()) < 1e-6 && g9.y() > g1.y());
    }

    // Layout in the background: positions arrive later; the old ones stay meanwhile.
    void layoutInBackground()
    {
        QTemporaryDir tmp;
        QVector<QPair<int, int>> edges;
        for (int r = 1; r < 64; ++r)
            edges << QPair<int, int>{r, r & (r - 1)};
        const QString anchor = writeEdgesOtf2(tmp.path(), 64, edges, 2);
        QVERIFY(!anchor.isEmpty());
        TraceTimelineModel model;
        model.addSnapshot({build(anchor)});
        TraceGraphItem item;
        item.setWidth(600);
        item.setHeight(500);
        item.setModel(&model);
        item.setLevel(QStringLiteral("ranks"));
        QTRY_VERIFY(!item.layoutBusy());
        QCOMPARE(item.effectiveLayout(), QStringLiteral("tree")); // a binomial tree
    }

    // Reference window: comparison findings and the "change" line colour.
    void referenceComparison()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        model.addSnapshot({build(m_ring4)});
        item.setModel(&model);
        item.setWidth(600);
        item.setHeight(400);
        item.setLevel(QStringLiteral("ranks"));
        item.setViewStartNs(0);
        item.setViewEndNs(1000 * kMs);
        QVERIFY(!item.hasReference());
        QSignalSpy refChanged(&item, &TraceGraphItem::referenceChanged);
        item.setReference();
        QVERIFY(item.hasReference());
        QCOMPARE(refChanged.count(), 1);
        QVERIFY(!item.referenceLabel().isEmpty());
        // Another window: comparison findings appear.
        item.setViewStartNs(1000 * kMs);
        item.setViewEndNs(2500 * kMs);
        bool comparison = false;
        for (const QVariant &v : item.findingMaps())
            comparison = comparison || v.toMap().value(QStringLiteral("category")).toString() == QLatin1String("comparison");
        QVERIFY(comparison);
        item.setEdgeColor(QStringLiteral("change"));
        QCOMPARE(item.edgeColor(), QStringLiteral("change"));
        QCOMPARE(item.edgeLegend().value(QStringLiteral("colors")).toStringList().size(), 3);
        QVERIFY(!render(item).isNull());
        item.clearReference();
        QVERIFY(!item.hasReference());
        QCOMPARE(item.edgeColor(), QStringLiteral("wait")); // "change" needs a reference
    }

    // A link clicked in the graph: the timeline draws only that link's messages.
    void timelineHighlightsLink()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_ring4)});
        TraceTimelineItem tl;
        tl.setModel(&model);
        tl.setWidth(800);
        tl.setHeight(300);
        tl.setViewStartNs(0);
        tl.setViewEndNs(2500 * kMs);
        tl.setLinkFrom(3);
        tl.setLinkTo(0);
        QVERIFY(!tl.arrowMessages().isEmpty());
        const auto store = model.detail();
        QVERIFY(store);
        for (int i : tl.arrowMessages()) {
            QCOMPARE(store->messages[i].sender, 3);
            QCOMPARE(store->messages[i].receiver, 0);
        }
        tl.setLinkFrom(-1);
        QVERIFY(tl.arrowMessages().isEmpty() || tl.selectedRank() >= 0 || tl.allArrows());

        // The clicked edge carries its message with the longest waiting (jump target).
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        item.setModel(&model);
        item.setWidth(600);
        item.setHeight(400);
        item.setViewStartNs(0);
        item.setViewEndNs(1000 * kMs);
        const QPointF mid = item.edgeMidpoint(edgeIndex(item.graph(), 3, 0));
        QSignalSpy clicked(&item, &TraceGraphItem::edgeClicked);
        item.clickAt(mid.x(), mid.y());
        QCOMPARE(clicked.count(), 1);
        const QVariantMap d = clicked.first().first().toMap();
        QVERIFY(d.value(QStringLiteral("longestSendNs")).toLongLong() >= 0);
        QVERIFY(d.value(QStringLiteral("longestRecvNs")).toLongLong() >= d.value(QStringLiteral("longestSendNs")).toLongLong());
        QVERIFY(d.value(QStringLiteral("longestWaitSeconds")).toDouble() > 0.2);
    }

    // Stepping from collective call to collective call.
    void stepsThroughCollectives()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        model.addSnapshot({build(m_ring4)});
        item.setModel(&model);
        item.setWidth(600);
        item.setHeight(400);
        item.setViewStartNs(0);
        item.setViewEndNs(2500 * kMs);
        QCOMPARE(item.collectiveCount(), 1);
        const QVariantMap next = item.stepCollective(1);
        QVERIFY(next.value(QStringLiteral("found")).toBool());
        QCOMPARE(next.value(QStringLiteral("lastRank")).toInt(), 2);
        QVERIFY(next.value(QStringLiteral("fromNs")).toLongLong() < 1200 * kMs);
        QVERIFY(next.value(QStringLiteral("toNs")).toLongLong() > 1200 * kMs);
        QVERIFY(next.value(QStringLiteral("label")).toString().contains(QStringLiteral("MPI_Allreduce")));
        // Arrival colours: rank 2 (last) red, the first white.
        item.setNodeColor(QStringLiteral("arrival"));
        QCOMPARE(item.nodeColor(), QStringLiteral("arrival"));
        QVERIFY(item.arrivalSeconds(2) > 0.1);
        QVERIFY(item.nodeLegend().value(QStringLiteral("high")).toString().contains(QStringLiteral("ms")));
        item.setNodeColor(QStringLiteral("ratio"));
        QCOMPARE(item.nodeLegend().value(QStringLiteral("low")).toString(), QStringLiteral("receives"));
        // Critical path in gold.
        item.setShowCriticalPath(true);
        QVERIFY(item.showCriticalPath());
        QVERIFY(!item.criticalPathOfWindow().ranks.isEmpty());
        QVERIFY(!render(item).isNull());
        item.setShowCriticalPath(false);
        // The view follows (as TraceGraph.qml does); from there no earlier / later one.
        item.setViewStartNs(next.value(QStringLiteral("fromNs")).toLongLong());
        item.setViewEndNs(next.value(QStringLiteral("toNs")).toLongLong());
        QVERIFY(!item.stepCollective(-1).value(QStringLiteral("found")).toBool());
        QVERIFY(!item.stepCollective(1).value(QStringLiteral("found")).toBool());
    }

    // Wait colours span the window's least to most waiting rank (white to blue).
    void waitScaleFromLeastToMost()
    {
        TraceTimelineModel model;
        TraceGraphItem item;
        item.setSynchronousLayout(true);
        item.setLayout(QStringLiteral("circle"));
        setupRing4(model, item);
        QCOMPARE(item.minWaitSeconds(), 0.0);
        QVERIFY(item.maxWaitSeconds() > item.minWaitSeconds());
    }

    // TraceGraph.qml as its own view and below the timeline: loads, follows traceState.
    void traceGraphQmlFollowsTraceState()
    {
        const QString qmlFile = QFINDTESTDATA("../TraceGraph.qml");
        QVERIFY2(!qmlFile.isEmpty(), "TraceGraph.qml not found");
        qmlRegisterType<TraceGraphItem>("GUI_Cluster", 1, 0, "TraceGraphItem");
        qmlRegisterType<TraceTimelineItem>("GUI_Cluster", 1, 0, "TraceTimelineItem");
        TraceTimelineModel model;
        model.addSnapshot({build(m_ring4)});
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("tlModel"), &model);
        QQmlComponent c(&engine);
        c.setData("import QtQuick\n"
                  "Item {\n"
                  "  width: 1000; height: 1200\n"
                  "  property QtObject state: QtObject {\n"
                  "    property real viewStartNs: 0\n"
                  "    property real viewEndNs: 1000000000\n"
                  "    property int selectedRank: -1\n"
                  "    property bool showArrows: true\n"
                  "    property bool allArrows: false\n"
                  "    property bool highlightWait: true\n"
                  "    property bool showPhases: false\n"
                  "    property string phaseLevel: 'outer'\n"
                  "    property var lastBar: null\n"
                  "    property bool graphBelowTimeline: false\n"
                  "    property bool graphP2P: true\n"
                  "    property bool graphCollectives: true\n"
                  "    property bool graphDirected: true\n"
                  "    property string graphWeight: 'bytes'\n"
                  "    property string graphLevel: 'ranks'\n"
                  "    property string graphCommunicators: 'auto'\n"
                  "    property string graphLayout: 'circle'\n"
                  "    property int graphMaxEdges: 20000\n"
                  "    property var lastEdge: null\n"
                  "    signal viewRequested(real startNs, real endNs)\n"
                  "  }\n"
                  "  TraceGraph { objectName: 'own'; width: 1000; height: 400; listNodes: ({timeline: tlModel}); traceState: parent.state }\n"
                  "  TraceTimeline { objectName: 'tl'; y: 400; width: 1000; height: 800; listNodes: ({timeline: tlModel}); traceState: parent.state }\n"
                  "}\n",
                  QUrl::fromLocalFile(QFileInfo(qmlFile).absolutePath() + QStringLiteral("/__graph_test.qml")));
        std::unique_ptr<QObject> obj(c.create());
        QVERIFY2(obj, qPrintable(c.errorString()));
        QQuickWindow win;
        win.resize(1000, 1200);
        auto *rootItem = qobject_cast<QQuickItem *>(obj.get());
        rootItem->setParentItem(win.contentItem());
        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QObject *state = obj->property("state").value<QObject *>();
        QVERIFY(state);

        auto *own = obj->findChild<QQuickItem *>(QStringLiteral("own"))->findChild<TraceGraphItem *>();
        QVERIFY(own);
        QCOMPARE(own->edgeCount(), 4);

        // Simple mode by default; "More options" shows the rest; help opens.
        QObject *ownView = obj->findChild<QQuickItem *>(QStringLiteral("own"));
        QCOMPARE(ownView->property("advanced").toBool(), false);
        QObject *more = ownView->findChild<QObject *>(QStringLiteral("moreOptionsButton"));
        QVERIFY(more);
        QVERIFY(QMetaObject::invokeMethod(more, "clicked"));
        QCOMPARE(ownView->property("advanced").toBool(), true);
        QObject *help = ownView->findChild<QObject *>(QStringLiteral("helpPopup"));
        QVERIFY(help);
        QVERIFY(QMetaObject::invokeMethod(help, "open"));
        QTRY_VERIFY(help->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(help, "close"));
        QVERIFY(ownView->findChild<QObject *>(QStringLiteral("summaryCard")));

        // Questions set colours / filter; the same one again goes back to the default view.
        QVERIFY(QMetaObject::invokeMethod(ownView, "askQuestion", Q_ARG(QVariant, QStringLiteral("load"))));
        QCOMPARE(own->nodeColor(), QStringLiteral("compute"));
        QCOMPARE(ownView->property("activeQuestion").toString(), QStringLiteral("load"));
        QVERIFY(!ownView->property("questionAnswer").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(ownView, "askQuestion", Q_ARG(QVariant, QStringLiteral("mapping"))));
        QCOMPARE(own->edgeColor(), QStringLiteral("locality"));
        QCOMPARE(own->edgeFilter(), QStringLiteral("between"));
        QVERIFY(QMetaObject::invokeMethod(ownView, "askQuestion", Q_ARG(QVariant, QStringLiteral("runtime"))));
        QVERIFY(own->showCriticalPath());
        QVERIFY(QMetaObject::invokeMethod(ownView, "askQuestion", Q_ARG(QVariant, QStringLiteral("runtime"))));
        QVERIFY(!own->showCriticalPath());
        QCOMPARE(own->edgeColor(), QStringLiteral("wait"));
        QCOMPARE(own->nodeColor(), QStringLiteral("wait"));
        QCOMPARE(own->edgeFilter(), QStringLiteral("all"));
        QCOMPARE(ownView->property("activeQuestion").toString(), QString());

        // Options from traceState.
        state->setProperty("graphCollectives", false);
        state->setProperty("viewEndNs", 2500.0 * kMs);
        QCOMPARE(own->edgeCount(), 5); // ring + 3 -> 2 (1 -> 2 adds to the ring's edge), no star
        state->setProperty("graphWeight", QStringLiteral("messages"));
        QCOMPARE(own->weight(), QStringLiteral("messages"));
        state->setProperty("graphLayout", QStringLiteral("grid"));
        QCOMPARE(own->layout(), QStringLiteral("grid"));
        state->setProperty("graphLayout", QStringLiteral("circle"));
        QCOMPARE(own->effectiveLayout(), QStringLiteral("circle"));

        // Own view: the overview bar moves the window through viewRequested.
        QMetaObject::invokeMethod(state, "viewRequested", Q_ARG(double, 1000.0 * kMs), Q_ARG(double, 2000.0 * kMs));
        QCOMPARE(state->property("viewStartNs").toDouble(), 1000.0 * kMs);
        QCOMPARE(own->viewStartNs(), 1000 * kMs);

        // Clicking a rank in the graph selects it in traceState (and so in the timeline).
        const QPointF p = own->nodePosition(own->graph().indexOfRank(1));
        own->clickAt(p.x(), p.y());
        QCOMPARE(state->property("selectedRank").toInt(), 1);
        auto *timeline = obj->findChild<TraceTimelineItem *>();
        QVERIFY(timeline);
        QCOMPARE(timeline->selectedRank(), 1);

        // Graph below the timeline: created on demand, same state.
        QCOMPARE(obj->findChild<QQuickItem *>(QStringLiteral("tl"))->findChildren<TraceGraphItem *>().size(), 0);
        state->setProperty("graphBelowTimeline", true);
        QTRY_COMPARE(obj->findChild<QQuickItem *>(QStringLiteral("tl"))->findChildren<TraceGraphItem *>().size(), 1);
        auto *below = obj->findChild<QQuickItem *>(QStringLiteral("tl"))->findChildren<TraceGraphItem *>().first();
        QCOMPARE(below->selectedRank(), 1);
        QTRY_VERIFY2(below->height() >= 100, qPrintable(QString::number(below->height())));
        // Edge clicked below the timeline: details land in traceState.
        state->setProperty("graphCollectives", true);
        QVERIFY(below->edgeCount() > 0);
        const int e = below->drawnEdges().last();
        const QPointF mid = below->edgeMidpoint(e);
        QSignalSpy clicked(below, &TraceGraphItem::edgeClicked);
        below->clickAt(mid.x(), mid.y());
        QCOMPARE(clicked.count(), 1);
        // property var holds the map as a JS object.
        QVariant last = state->property("lastEdge");
        if (last.canConvert<QJSValue>())
            last = last.value<QJSValue>().toVariant();
        QCOMPARE(last.toMap().value(QStringLiteral("kind")),
                 clicked.first().first().toMap().value(QStringLiteral("kind")));
    }
};

QTEST_MAIN(TstTraceGraphItem)
#include "tst_trace_graph_item.moc"
