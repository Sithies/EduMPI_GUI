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
        // P2P edges are green, with or without waiting.
        for (const auto &pair : {std::pair{3, 0}, std::pair{0, 1}}) {
            const QPointF mid = item.edgeMidpoint(edgeIndex(item.graph(), pair.first, pair.second));
            const QColor c(img.pixel(int(mid.x()), int(mid.y())));
            QVERIFY2(c.green() > c.red() + 60 && c.green() > c.blue() + 60, qPrintable(c.name()));
        }
    }

    void wholeRunWhenWindowEmpty()
    {
        TraceTimelineModel model;
        model.addSnapshot({build(m_ring4)});
        TraceGraphItem item;
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
                  "    property string graphLayout: 'circle'\n"
                  "    property int graphMaxEdges: 500\n"
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

        // Options from traceState.
        state->setProperty("graphCollectives", false);
        state->setProperty("viewEndNs", 2500.0 * kMs);
        QCOMPARE(own->edgeCount(), 5); // ring + 3 -> 2 (1 -> 2 adds to the ring's edge), no star
        state->setProperty("graphWeight", QStringLiteral("messages"));
        QCOMPARE(own->weight(), QStringLiteral("messages"));

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
