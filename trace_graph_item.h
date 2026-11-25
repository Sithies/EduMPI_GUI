#pragma once

#include <QPointer>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "graph_model.h"

class TraceTimelineModel;

// Communication graph of a trace window (TraceTimelineModel): ranks (or nodes) as graph nodes,
// P2P messages as edges, collectives as star edges to one node per communicator (see
// trace/graph_model.h for the counting rules). Edge width = the chosen weight (bytes, messages
// or waiting time), edge colour = waiting time (late sender), collectives dashed.
// Compiled only with EDUMPI_TRACE; QML sees the model as a plain QObject*.
//
// Window: [viewStartNs, viewEndNs); an empty window means the whole run. The graph needs the
// messages of the window: a complete trace has them, a windowed one loads them on demand like
// the timeline (model.requestDetail), only while the item is visible and only for windows of at
// most maxDetailWindowNs; otherwise status tells why nothing is drawn.
class TraceGraphItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(qint64 viewStartNs READ viewStartNs WRITE setViewStartNs NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewEndNs READ viewEndNs WRITE setViewEndNs NOTIFY viewChanged)
    Q_PROPERTY(int selectedRank READ selectedRank WRITE setSelectedRank NOTIFY selectedRankChanged)
    Q_PROPERTY(bool showP2P READ showP2P WRITE setShowP2P NOTIFY displayChanged)
    Q_PROPERTY(bool showCollectives READ showCollectives WRITE setShowCollectives NOTIFY displayChanged)
    Q_PROPERTY(bool directed READ directed WRITE setDirected NOTIFY displayChanged)
    // "bytes", "messages" or "wait"
    Q_PROPERTY(QString weight READ weight WRITE setWeight NOTIFY displayChanged)
    // "ranks" or "nodes"
    Q_PROPERTY(QString level READ level WRITE setLevel NOTIFY displayChanged)
    // "circle" or "grid"
    Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY displayChanged)
    // Heaviest edges drawn (0 = all); edges of the selected rank are always drawn.
    Q_PROPERTY(int maxEdges READ maxEdges WRITE setMaxEdges NOTIFY displayChanged)
    // "" (graph drawn), "zoom" (window too long to load its messages), "dense" (too dense for the
    // memory budget), "loading" (messages are being loaded), "empty" (no trace yet)
    Q_PROPERTY(QString status READ status NOTIFY graphChanged)
    Q_PROPERTY(int edgeCount READ edgeCount NOTIFY graphChanged)
    Q_PROPERTY(int shownEdgeCount READ shownEdgeCount NOTIFY graphChanged)
    Q_PROPERTY(double maxWaitSeconds READ maxWaitSeconds NOTIFY graphChanged)

public:
    static constexpr int kDefaultMaxEdges = 500;
    static constexpr int kArrowHeadLimit = 300; // no arrow heads when more edges are drawn
    static constexpr int kLabelLimit = 64;      // rank labels only up to this many ranks

    explicit TraceGraphItem(QQuickItem *parent = nullptr);

    QObject *model() const;
    void setModel(QObject *model);
    qint64 viewStartNs() const { return m_viewStart; }
    void setViewStartNs(qint64 ns);
    qint64 viewEndNs() const { return m_viewEnd; }
    void setViewEndNs(qint64 ns);
    int selectedRank() const { return m_selectedRank; }
    void setSelectedRank(int rank);
    bool showP2P() const { return m_options.p2p; }
    void setShowP2P(bool on);
    bool showCollectives() const { return m_options.collectives; }
    void setShowCollectives(bool on);
    bool directed() const { return m_options.directed; }
    void setDirected(bool on);
    QString weight() const;
    void setWeight(const QString &weight);
    QString level() const;
    void setLevel(const QString &level);
    QString layout() const;
    void setLayout(const QString &layout);
    int maxEdges() const { return m_maxEdges; }
    void setMaxEdges(int n);
    QString status() const { return m_status; }
    int edgeCount() const { return int(m_graph.edges.size()); }
    int shownEdgeCount() const { return int(m_drawn.size()); }
    double maxWaitSeconds() const { return m_maxWait; }

    // Rank node: toggles selectedRank; edge: emits edgeClicked; elsewhere nothing.
    Q_INVOKABLE void clickAt(qreal x, qreal y);

    void paint(QPainter *painter) override;

    // For tests: pixel position of a graph node (index into the current graph), the middle of
    // an edge's curve (index into graph().edges), the graph and the drawn edges.
    QPointF nodePosition(int node) const;
    QPointF edgeMidpoint(int edge) const;
    const QVector<int> &drawnEdges() const { return m_drawn; }
    const trace::CommGraph &graph() const { return m_graph; }

signals:
    void modelChanged();
    void viewChanged();
    void selectedRankChanged();
    void displayChanged();
    void graphChanged();
    // Keys: kind ("p2p" / "collective"), from, to (labels), fromRank, toRank (-1 if not a rank),
    // comm (collectives), bytes, messages, waitSeconds, directed, fromNs, toNs.
    void edgeClicked(QVariantMap details);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    struct EdgePath {
        QPointF from;
        QPointF control;
        QPointF to;
    };

    void rebuild();          // graph, layout and drawn edges from model + window + options
    void reselect();         // drawn edges only (selection / maxEdges)
    void maybeRequestDetail();
    qint64 windowStart() const;
    qint64 windowEnd() const;
    QPointF center() const;
    double radius() const;
    double nodeRadius() const;
    EdgePath edgePath(const trace::GraphEdge &edge) const;
    int focusNode() const; // graph node of the selected rank, -1 if none
    int nodeAt(QPointF p) const;
    int edgeAt(QPointF p) const;
    QString nodeLabel(int node) const;
    double edgeWidth(const trace::GraphEdge &edge) const;
    void setStatus(const QString &status);

    QPointer<TraceTimelineModel> m_model;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    int m_selectedRank = -1;
    trace::GraphOptions m_options;
    trace::GraphWeight m_weight = trace::GraphWeight::Bytes;
    trace::GraphLayout m_layout = trace::GraphLayout::Circle;
    int m_maxEdges = kDefaultMaxEdges;

    trace::CommGraph m_graph;
    QVector<QPointF> m_unitPos; // layoutGraph() result
    QVector<int> m_drawn;       // indices into m_graph.edges, heaviest first
    double m_maxWeight = 0;
    double m_maxWait = 0;
    QString m_status = QStringLiteral("empty");

    bool m_requested = false;
    qint64 m_requestedFrom = 0;
    qint64 m_requestedTo = 0;
};
