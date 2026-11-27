// Renders the Trace Graph view (TraceGraph.qml) of real trace windows into PNG files, without the
// GUI: for reviewing the graph on large traces (e.g. tools/big-trace). Each window is read once
// (early stop after its end); each --view renders the view with other traceState settings.
//
//   trace_graph_snapshot --trace .../traces.otf2 --out DIR --window 2000:2010 [--window ...]
//       [--view NAME:graphLevel=nodes,graphWeight=wait ...] [--size 1400x900]
//
// Windows in milliseconds. Without --view one image with the defaults is written per window.
// Output: DIR/<from>-<to>ms_<view>.png, plus one line per image on stdout.
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

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

std::shared_ptr<TimelineStore> buildWindow(const QString &anchor, qint64 from, qint64 to, QString *error)
{
    TraceAggregator agg;
    TimelineBuilder builder(agg, from, to, true, 2);
    TraceMeta meta;
    if (!readOtf2(anchor, builder, &meta, error))
        return {};
    agg.finish(meta.traceLengthNs);
    return builder.finish();
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

const char *kStateQml = R"(import QtQuick
Item {
  property QtObject state: QtObject {
    property real viewStartNs: 0
    property real viewEndNs: 0
    property int selectedRank: -1
    property bool showArrows: true
    property bool allArrows: false
    property bool highlightWait: true
    property bool showPhases: false
    property string phaseLevel: 'outer'
    property var lastBar: null
    property bool graphBelowTimeline: false
    property bool graphP2P: true
    property bool graphCollectives: true
    property bool graphDirected: true
    property string graphWeight: 'bytes'
    property string graphLevel: 'auto'
    property string graphCommunicators: 'auto'
    property string graphLayout: 'auto'
    property int graphMaxEdges: 20000
    property var lastEdge: null
    property int graphLinkFrom: -1
    property int graphLinkTo: -1
    property string graphEdgeColor: 'wait'
    property string graphNodeColor: 'wait'
    property string graphEdgeFilter: 'all'
    signal viewRequested(real startNs, real endNs)
  }
  TraceGraph { objectName: 'graph'; anchors.fill: parent; listNodes: ({timeline: tlModel}); traceState: parent.state }
}
)";

// Synthetic patterns (tests/otf2_fixture.h writeEdgesOtf2) for layouts without the trace
// generator: ring, tree (binomial reduce), clusters (all-to-all blocks chained), random
// (3 random partners each), master (master-worker), stencil (2D, not periodic), torus3d.
QVector<QPair<int, int>> syntheticEdges(const QString &name, int ranks)
{
    QVector<QPair<int, int>> e;
    QRandomGenerator rng(7);
    if (name == QLatin1String("ring")) {
        for (int r = 0; r < ranks; ++r) {
            e.append({r, (r + 1) % ranks});
            e.append({(r + 1) % ranks, r});
        }
    } else if (name == QLatin1String("tree")) {
        for (int r = 1; r < ranks; ++r)
            e.append({r, r & (r - 1)}); // binomial tree: r -> r without its lowest bit
    } else if (name == QLatin1String("clusters")) {
        const int block = 16;
        for (int b = 0; b * block < ranks; ++b) {
            for (int i = b * block; i < std::min(ranks, (b + 1) * block); ++i)
                for (int j = b * block; j < std::min(ranks, (b + 1) * block); ++j)
                    if (i != j)
                        e.append({i, j});
            if ((b + 1) * block < ranks)
                e.append({b * block, (b + 1) * block});
        }
    } else if (name == QLatin1String("random")) {
        for (int r = 0; r < ranks; ++r)
            for (int k = 0; k < 3; ++k) {
                const int t = int(rng.bounded(ranks));
                if (t != r)
                    e.append({r, t});
            }
    } else if (name == QLatin1String("master")) {
        for (int r = 1; r < ranks; ++r) {
            e.append({0, r});
            e.append({r, 0});
        }
    } else if (name == QLatin1String("stencil")) {
        const int cols = 16;
        for (int r = 0; r < ranks; ++r) {
            if (r % cols + 1 < cols && r + 1 < ranks)
                e.append({r, r + 1}), e.append({r + 1, r});
            if (r + cols < ranks)
                e.append({r, r + cols}), e.append({r + cols, r});
        }
    } else if (name == QLatin1String("torus2d") || name == QLatin1String("torus2d-shuffled")) {
        // 16-wide periodic grid; "-shuffled": ranks numbered randomly (no pattern to detect).
        const int cols = 16, rows = std::max(1, ranks / cols);
        QVector<int> perm(cols * rows);
        for (int i = 0; i < perm.size(); ++i)
            perm[i] = i;
        if (name.endsWith(QLatin1String("shuffled")))
            for (int i = int(perm.size()) - 1; i > 0; --i)
                std::swap(perm[i], perm[int(rng.bounded(i + 1))]);
        for (int r = 0; r < cols * rows; ++r) {
            const int x = r % cols, y = r / cols;
            e.append({perm[r], perm[y * cols + (x + 1) % cols]});
            e.append({perm[r], perm[((y + 1) % rows) * cols + x]});
            e.append({perm[y * cols + (x + 1) % cols], perm[r]});
            e.append({perm[((y + 1) % rows) * cols + x], perm[r]});
        }
    } else if (name == QLatin1String("torus3d")) {
        const int k = 4; // 4 x 4 x (ranks / 16)
        const int z = std::max(1, ranks / (k * k));
        for (int r = 0; r < k * k * z; ++r) {
            const int x = r % k, y = (r / k) % k, w = r / (k * k);
            e.append({r, (x + 1) % k + y * k + w * k * k});
            e.append({r, x + ((y + 1) % k) * k + w * k * k});
            e.append({r, x + y * k + ((w + 1) % z) * k * k});
        }
    }
    return e;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCommandLineParser p;
    p.addHelpOption();
    p.addOption({QStringLiteral("trace"), QStringLiteral("traces.otf2"), QStringLiteral("file")});
    p.addOption({QStringLiteral("out"), QStringLiteral("output directory"), QStringLiteral("dir")});
    p.addOption({QStringLiteral("qml"), QStringLiteral("directory of TraceGraph.qml"), QStringLiteral("dir"),
                 QStringLiteral(EDUMPI_SOURCE_DIR)});
    p.addOption({QStringLiteral("window"), QStringLiteral("FROM:TO in ms (repeatable)"), QStringLiteral("ms")});
    p.addOption({QStringLiteral("view"), QStringLiteral("NAME:key=value,... (repeatable)"), QStringLiteral("view")});
    p.addOption({QStringLiteral("size"), QStringLiteral("WxH"), QStringLiteral("size"), QStringLiteral("1400x900")});
    p.addOption({QStringLiteral("synthetic"), QStringLiteral("pattern instead of --trace: ring, tree, clusters, random, master, stencil, torus3d"),
                 QStringLiteral("name")});
    p.addOption({QStringLiteral("ranks"), QStringLiteral("ranks of a synthetic pattern"), QStringLiteral("n"), QStringLiteral("128")});
    p.process(app);

    QString anchor = p.value(QStringLiteral("trace"));
    QTemporaryDir synthDir;
    if (p.isSet(QStringLiteral("synthetic"))) {
        const QString name = p.value(QStringLiteral("synthetic"));
        anchor = writeEdgesOtf2(synthDir.filePath(name), p.value(QStringLiteral("ranks")).toInt(),
                                syntheticEdges(name, p.value(QStringLiteral("ranks")).toInt()), 3);
        if (anchor.isEmpty()) {
            std::fprintf(stderr, "cannot write the synthetic trace %s\n", qPrintable(name));
            return 1;
        }
    }
    const QDir out(p.value(QStringLiteral("out")));
    QDir().mkpath(out.absolutePath());
    const QStringList size = p.value(QStringLiteral("size")).split(QLatin1Char('x'));
    const int width = size.value(0).toInt(), height = size.value(1).toInt();
    QStringList views = p.values(QStringLiteral("view"));
    if (views.isEmpty())
        views << QStringLiteral("default:");

    qmlRegisterType<TraceGraphItem>("GUI_Cluster", 1, 0, "TraceGraphItem");
    qmlRegisterType<TraceTimelineItem>("GUI_Cluster", 1, 0, "TraceTimelineItem");

    for (const QString &w : p.values(QStringLiteral("window"))) {
        const qint64 from = qint64(w.section(QLatin1Char(':'), 0, 0).toDouble() * kMs);
        const qint64 to = qint64(w.section(QLatin1Char(':'), 1, 1).toDouble() * kMs);
        QElapsedTimer t;
        t.start();
        QString error;
        auto store = buildWindow(anchor, from, to, &error);
        if (!store) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 1;
        }
        const qint64 readMs = t.elapsed();

        TraceTimelineModel model;
        model.addSnapshot({store});
        model.addSnapshot({store, from, to});
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("tlModel"), &model);
        QQmlComponent c(&engine);
        c.setData(kStateQml, QUrl::fromLocalFile(p.value(QStringLiteral("qml")) + QStringLiteral("/__graph_snapshot.qml")));
        std::unique_ptr<QObject> obj(c.create());
        if (!obj) {
            std::fprintf(stderr, "%s\n", qPrintable(c.errorString()));
            return 1;
        }
        QQuickWindow win;
        win.resize(width, height);
        auto *root = qobject_cast<QQuickItem *>(obj.get());
        root->setParentItem(win.contentItem());
        root->setSize(QSizeF(width, height));
        win.show();
        QObject *state = obj->property("state").value<QObject *>();
        auto *graph = obj->findChild<TraceGraphItem *>();
        if (graph)
            graph->setSynchronousLayout(true);
        state->setProperty("viewStartNs", double(from));
        state->setProperty("viewEndNs", double(to));

        for (const QString &v : std::as_const(views)) {
            const QString name = v.section(QLatin1Char(':'), 0, 0);
            const QStringList sets = v.section(QLatin1Char(':'), 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &kv : sets) {
                const QString key = kv.section(QLatin1Char('='), 0, 0);
                if (key == QLatin1String("zoomToFinding") && graph) {
                    graph->zoomToFinding(kv.section(QLatin1Char('='), 1).toInt());
                    continue;
                }
                if ((key == QLatin1String("pin") || key == QLatin1String("pinKind")) && graph) {
                    // As a click on a card: opened, highlighted, zoomed (by index or by kind).
                    int idx = kv.section(QLatin1Char('='), 1).toInt();
                    if (key == QLatin1String("pinKind")) {
                        idx = -1;
                        const QVariantList list = graph->findingMaps();
                        for (int k = 0; k < list.size(); ++k)
                            if (list[k].toMap().value(QStringLiteral("kind")).toString() == kv.section(QLatin1Char('='), 1))
                                idx = k;
                    }
                    if (QObject *panel = obj->findChild<QObject *>(QStringLiteral("findingsPanel")))
                        panel->setProperty("pinned", idx);
                    graph->zoomToFinding(idx);
                    continue;
                }
                if (key == QLatin1String("critical") && graph) {
                    graph->setShowCriticalPath(kv.section(QLatin1Char('='), 1) == QLatin1String("true"));
                    continue;
                }
                if (key == QLatin1String("step") && graph) {
                    const QVariantMap r = graph->stepCollective(kv.section(QLatin1Char('='), 1).toInt());
                    if (r.value(QStringLiteral("found")).toBool()) {
                        state->setProperty("viewStartNs", r.value(QStringLiteral("fromNs")).toDouble());
                        state->setProperty("viewEndNs", r.value(QStringLiteral("toNs")).toDouble());
                        state->setProperty("selectedRank", r.value(QStringLiteral("lastRank")).toInt());
                        state->setProperty("graphNodeColor", QStringLiteral("arrival"));
                        std::printf("step: %s\n", qPrintable(r.value(QStringLiteral("label")).toString()));
                    }
                    continue;
                }
                if (key == QLatin1String("reference") && graph) {
                    graph->setReference();
                    continue;
                }
                if (key == QLatin1String("highlight") && graph) {
                    graph->setHighlightedFinding(kv.section(QLatin1Char('='), 1).toInt());
                    continue;
                }
                if (key == QLatin1String("resetView") && graph) {
                    graph->resetView();
                    graph->setHighlightedFinding(-1);
                    continue;
                }
                const QString val = kv.section(QLatin1Char('='), 1);
                bool isNum = false;
                const double num = val.toDouble(&isNum);
                state->setProperty(key.toUtf8().constData(),
                                   isNum ? QVariant(num) : (val == QLatin1String("true") || val == QLatin1String("false")
                                                                ? QVariant(val == QLatin1String("true")) : QVariant(val)));
            }
            spin(400);
            const QImage img = win.grabWindow();
            const QString prefix = p.isSet(QStringLiteral("synthetic")) ? p.value(QStringLiteral("synthetic"))
                                                                         : QStringLiteral("%1-%2ms").arg(from / kMs).arg(to / kMs);
            const QString file = out.filePath(QStringLiteral("%1_%2.png").arg(prefix, name));
            img.save(file);
            std::printf("%s  read %lld ms  status '%s' level %s layout %s edges %d shown %d\n", qPrintable(file),
                        (long long)readMs, qPrintable(graph ? graph->status() : QStringLiteral("?")),
                        qPrintable(graph ? graph->effectiveLevel() : QString()),
                        qPrintable(graph ? graph->effectiveLayout() : QString()),
                        graph ? graph->edgeCount() : -1, graph ? graph->shownEdgeCount() : -1);
            std::fflush(stdout);
        }
    }
    return 0;
}
