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
#include <QTimer>

#include <cstdio>
#include <memory>

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
    property string graphLayout: 'circle'
    property int graphMaxEdges: 500
    property string graphView: 'auto'
    property var lastEdge: null
    signal viewRequested(real startNs, real endNs)
  }
  TraceGraph { objectName: 'graph'; anchors.fill: parent; listNodes: ({timeline: tlModel}); traceState: parent.state }
}
)";

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
    p.process(app);

    const QString anchor = p.value(QStringLiteral("trace"));
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
        state->setProperty("viewStartNs", double(from));
        state->setProperty("viewEndNs", double(to));
        auto *graph = obj->findChild<TraceGraphItem *>();

        for (const QString &v : std::as_const(views)) {
            const QString name = v.section(QLatin1Char(':'), 0, 0);
            const QStringList sets = v.section(QLatin1Char(':'), 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &kv : sets) {
                const QString key = kv.section(QLatin1Char('='), 0, 0);
                const QString val = kv.section(QLatin1Char('='), 1);
                bool isNum = false;
                const double num = val.toDouble(&isNum);
                state->setProperty(key.toUtf8().constData(),
                                   isNum ? QVariant(num) : (val == QLatin1String("true") || val == QLatin1String("false")
                                                                ? QVariant(val == QLatin1String("true")) : QVariant(val)));
            }
            spin(400);
            const QImage img = win.grabWindow();
            const QString file = out.filePath(QStringLiteral("%1-%2ms_%3.png").arg(from / kMs).arg(to / kMs).arg(name));
            img.save(file);
            std::printf("%s  read %lld ms  status '%s' level %s edges %d shown %d\n", qPrintable(file),
                        (long long)readMs, qPrintable(graph ? graph->status() : QStringLiteral("?")),
                        qPrintable(graph ? graph->effectiveLevel() : QString()),
                        graph ? graph->edgeCount() : -1, graph ? graph->shownEdgeCount() : -1);
            std::fflush(stdout);
        }
    }
    return 0;
}
