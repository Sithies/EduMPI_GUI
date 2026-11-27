#pragma once

#include <QPointer>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QHash>
#include <QTimer>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

#include "graph_model.h"

class TraceTimelineModel;

// Communication graph of a trace window (TraceTimelineModel): ranks (or nodes) as graph nodes,
// P2P messages as edges, collectives as star edges to one node per communicator (see
// trace/graph_model.h for the counting rules). Colours follow the 3D view: P2P edges green,
// collective edges black (dashed), node fill = waiting time of the rank (late sender, white to
// blue). Edge width = the chosen weight (bytes, messages or waiting time).
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
    // "auto" (default), "ranks" or "nodes". Auto: ranks while the window is short (zoomed in,
    // single steps) or the trace small, compute nodes for long windows of many ranks.
    Q_PROPERTY(QString level READ level WRITE setLevel NOTIFY displayChanged)
    // Level actually drawn: "ranks" or "nodes".
    Q_PROPERTY(QString effectiveLevel READ effectiveLevel NOTIFY graphChanged)
    // "auto" (default), "separate" or "merged": one node per communicator or one for all.
    // Auto merges when there are more than kAutoCommunicatorLimit communicators.
    Q_PROPERTY(QString communicators READ communicators WRITE setCommunicators NOTIFY displayChanged)
    Q_PROPERTY(bool communicatorsMerged READ communicatorsMerged NOTIFY graphChanged)
    // Compute nodes opened into their ranks (double-click, nodes level only).
    Q_PROPERTY(int expandedCount READ expandedCount NOTIFY graphChanged)
    // "auto" (default), "circle" or "grid". Auto: the process grid when the ranks form a 2D
    // stencil (trace::detectPattern), the circle otherwise. Grid with a detected stencil places
    // the ranks by their grid position.
    Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY displayChanged)
    Q_PROPERTY(QString effectiveLayout READ effectiveLayout NOTIFY graphChanged)
    // View transform: zoom (1 = fit) and pan in pixels; wheel / pinch / drag in QML.
    Q_PROPERTY(double zoom READ zoom NOTIFY viewTransformChanged)
    // Heaviest edges drawn (0 = all); edges of the selected rank are always drawn.
    Q_PROPERTY(int maxEdges READ maxEdges WRITE setMaxEdges NOTIFY displayChanged)
    // "" (graph drawn), "zoom" (window too long to load its messages), "dense" (too dense for the
    // memory budget), "loading" (messages are being loaded), "empty" (no trace yet)
    Q_PROPERTY(QString status READ status NOTIFY graphChanged)
    Q_PROPERTY(int edgeCount READ edgeCount NOTIFY graphChanged)
    Q_PROPERTY(int shownEdgeCount READ shownEdgeCount NOTIFY graphChanged)
    // Of those, drawn strong and coloured (the rest thin and pale as context).
    Q_PROPERTY(int notableEdgeCount READ notableEdgeCount NOTIFY graphChanged)
    // Largest waiting time (late sender) of one rank / host in the window: the blue end of the scale.
    Q_PROPERTY(double maxWaitSeconds READ maxWaitSeconds NOTIFY graphChanged)
    // Smallest waiting time of one rank / host: the white end of the scale (so that differences
    // show even when every rank waits a lot, e.g. in long windows).
    Q_PROPERTY(double minWaitSeconds READ minWaitSeconds NOTIFY graphChanged)
    // Findings of the window (trace::analyzeCommGraph on the ranks): pattern, locality, waiting
    // outliers, slow nodes, late senders. findings: one line each; findingList: maps with kind,
    // title, metric, detail, severity (0..2), rank, ranks, nodes, fromRank, toRank.
    Q_PROPERTY(QStringList findings READ findings NOTIFY graphChanged)
    Q_PROPERTY(QVariantList findingRanks READ findingRanks NOTIFY graphChanged)
    Q_PROPERTY(QVariantList findingList READ findingMaps NOTIFY graphChanged)
    // One sentence over the findings: {text, finding (index into findingList, -1), severity}.
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY graphChanged)
    // Edge colour: "wait" (default; waiting on the link, green -> yellow -> red), "volume",
    // "size" (average message size), "locality" (inside / between compute nodes), "change"
    // (waiting against the reference window). Edges inside a compute node are drawn lighter.
    Q_PROPERTY(QString edgeColor READ edgeColor WRITE setEdgeColor NOTIFY displayChanged)
    // Node fill: "wait" (default, white -> blue), "compute" (time outside MPI, white -> orange),
    // "ratio" (share of sent bytes: blue = receives, white = balanced, orange = sends) or
    // "arrival" (arrival at the collective stepped to: white = first, red = last).
    Q_PROPERTY(QString nodeColor READ nodeColor WRITE setNodeColor NOTIFY displayChanged)
    // Legend of the colours: maps with label, low, high (texts), colors (list of colour names).
    Q_PROPERTY(QVariantMap edgeLegend READ edgeLegend NOTIFY graphChanged)
    Q_PROPERTY(QVariantMap nodeLegend READ nodeLegend NOTIFY graphChanged)
    // Edge filter: "all", "waiting" (only links with waiting), "between" (only between compute
    // nodes), "inside" (only inside compute nodes).
    Q_PROPERTY(QString edgeFilter READ edgeFilter WRITE setEdgeFilter NOTIFY displayChanged)
    // Reference window for comparisons (setReference / clearReference).
    Q_PROPERTY(bool hasReference READ hasReference NOTIFY referenceChanged)
    // Collective calls in the window (stepCollective).
    Q_PROPERTY(int collectiveCount READ collectiveCount NOTIFY graphChanged)
    // The critical path of the window drawn in gold (ranks and hand-overs).
    Q_PROPERTY(bool showCriticalPath READ showCriticalPath WRITE setShowCriticalPath NOTIFY displayChanged)
    Q_PROPERTY(QString referenceLabel READ referenceLabel NOTIFY referenceChanged)
    // Finding whose ranks / link are highlighted (-1 = none); the rest of the graph is dimmed.
    Q_PROPERTY(int highlightedFinding READ highlightedFinding WRITE setHighlightedFinding NOTIFY highlightChanged)

public:
    // All edges are drawn up to this many: the heaviest kNotableEdges strong and coloured, the
    // rest thin and pale as context. Above it, Auto level switches to compute nodes.
    static constexpr int kDefaultMaxEdges = 20000;
    static constexpr int kNotableEdges = 300;
    static constexpr int kArrowHeadLimit = 300; // no arrow heads when more edges are drawn
    static constexpr int kLabelLimit = 64;      // rank labels only up to this many ranks (at zoom 1)
    static constexpr double kMaxZoom = 60;
    static constexpr double kLabelSpacing = 16; // rank labels once neighbours are this far apart
    // Auto level: ranks for at most this many ranks, or for windows up to kAutoRanksWindowNs.
    static constexpr int kAutoRankLimit = 64;
    static constexpr qint64 kAutoRanksWindowNs = 50'000'000; // 50 ms
    static constexpr int kAutoCommunicatorLimit = 8;

    // Hint to zoom in from this window length on (the graph sums up many steps).
    static constexpr qint64 kLongWindowNs = 1'000'000'000; // 1 s

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
    QString effectiveLevel() const;
    QString communicators() const;
    void setCommunicators(const QString &mode);
    bool communicatorsMerged() const { return m_commMerged; }
    int expandedCount() const { return int(m_options.expandedNodes.size()); }
    QString layout() const;
    void setLayout(const QString &layout);
    QString effectiveLayout() const;
    // Layouts computed on the calling thread (tests, snapshots); default: in the background.
    void setSynchronousLayout(bool on);
    bool layoutBusy() const { return m_layoutBusy; }
    double zoom() const { return m_zoom; }
    QPointF pan() const { return m_pan; }
    int highlightedFinding() const { return m_highlightFinding; }
    void setHighlightedFinding(int index);
    QVariantList findingMaps() const;
    QVariantMap summary() const;
    // Graph nodes highlighted by the current finding.
    const QVector<bool> &highlightedNodes() const { return m_highlightNodes; }

    // Zooms by factor around (x, y), keeping that point in place (1 .. kMaxZoom).
    Q_INVOKABLE void zoomAt(qreal x, qreal y, qreal factor);
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    Q_INVOKABLE void resetView();
    // Zooms to the ranks / nodes of a finding (and highlights it).
    Q_INVOKABLE void zoomToFinding(int index);
    // Zooms to a rank (its node at the nodes level); false if not in the graph.
    Q_INVOKABLE bool zoomToRank(int rank);
    // Steps to the next (direction > 0) / previous collective call of the loaded window:
    // returns {found, fromNs, toNs (a view around it), label, lastRank, function}; found = false
    // when the window has no further one (the caller then moves the window on).
    Q_INVOKABLE QVariantMap stepCollective(int direction);
    int collectiveCount() const { return int(m_collInstances.size()); }
    bool showCriticalPath() const { return m_showCritical; }
    void setShowCriticalPath(bool on);
    const trace::CriticalPath &criticalPathOfWindow() const { return m_critical; }
    // Arrival offset of a rank at the collective stepped to (seconds after the first; -1 = not in it).
    double arrivalSeconds(int rank) const;
    // The current window (ranks) as the reference for comparisons; clearReference drops it.
    Q_INVOKABLE void setReference();
    Q_INVOKABLE void clearReference();

    QString edgeColor() const { return m_edgeColor; }
    void setEdgeColor(const QString &mode);
    QString nodeColor() const { return m_nodeColor; }
    void setNodeColor(const QString &mode);
    QVariantMap edgeLegend() const;
    QVariantMap nodeLegend() const;
    QString edgeFilter() const { return m_edgeFilter; }
    void setEdgeFilter(const QString &filter);
    bool hasReference() const { return m_reference.rankNodeCount > 0; }
    QString referenceLabel() const { return m_referenceLabel; }
    // Colour of a drawn edge (for tests).
    QColor edgeColorOf(int edge) const;
    int maxEdges() const { return m_maxEdges; }
    void setMaxEdges(int n);
    QString status() const { return m_status; }
    int edgeCount() const { return int(m_graph.edges.size()); }
    int shownEdgeCount() const { return int(m_drawn.size()); }
    int notableEdgeCount() const { return int(m_drawn.size()) - m_notableFrom; }
    double maxWaitSeconds() const { return m_maxWait; }
    double minWaitSeconds() const { return m_minWait; }
    QStringList findings() const;
    QVariantList findingRanks() const;
    const QVector<trace::GraphFinding> &findingList() const { return m_findings; }

    double nodeWaitSeconds(int node) const { return m_nodeWait.value(node); }

    // Rank node: toggles selectedRank; edge: emits edgeClicked; elsewhere nothing.
    Q_INVOKABLE void clickAt(qreal x, qreal y);
    // Nodes level: a compute node opens into its ranks, a rank of an opened node closes it again.
    // Returns true if something changed.
    Q_INVOKABLE bool toggleExpandAt(qreal x, qreal y);
    Q_INVOKABLE void collapseAll();
    // Hover text for the node or edge at (x, y), "" elsewhere.
    Q_INVOKABLE QString toolTipAt(qreal x, qreal y) const;

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
    void viewTransformChanged();
    void referenceChanged();
    void highlightChanged();

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
    bool isWrapEdge(const trace::GraphEdge &e) const;
    int edgeAt(QPointF p) const;
    QString nodeLabel(int node) const;
    QPointF basePosition(int node) const; // position at zoom 1
    QPointF viewCenter() const { return center() + m_pan; }
    double nodeSpacing() const;           // distance of neighbouring rank / host nodes on screen
    void updateSpacing();                 // m_unitSpacing from m_unitPos
    void updateHighlight();
    void relayout(); // positions for m_graph (cache / background / synchronous)
    bool edgePassesFilter(const trace::GraphEdge &e) const;
    double computeSeconds(int node) const; // time outside MPI of a rank / host (per rank)
    void updateEdgeScale();
    void addTimeSeries(const trace::TimelineStore &store, qint64 from, qint64 to);
    void setPositions(const QVector<QPointF> &positions, trace::GraphLayout layout, bool animate);
    double edgeWidth(const trace::GraphEdge &edge) const;
    void setStatus(const QString &status);
    void paintHint(QPainter *painter);
    QColor waitColor(double seconds) const;
    QColor ratioColor(int node) const;
    QColor arrivalColor(int node) const;

    QPointer<TraceTimelineModel> m_model;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    int m_selectedRank = -1;
    trace::GraphOptions m_options;
    trace::GraphWeight m_weight = trace::GraphWeight::Bytes;
    trace::GraphLayout m_layout = trace::GraphLayout::Circle; // effective
    enum class Mode : quint8 { Auto, Off, On }; // level: On = nodes; communicators: On = merged
    Mode m_levelMode = Mode::Auto;
    Mode m_commMode = Mode::Auto;
    bool m_commMerged = false;
    enum class LayoutMode : quint8 { Auto, Fixed };
    LayoutMode m_layoutMode = LayoutMode::Auto;
    trace::GraphLayout m_fixedLayout = trace::GraphLayout::Circle;
    int m_gridColumns = 0;          // detected stencil grid (0 = none)
    bool m_syncLayout = false;
    bool m_layoutBusy = false;
    quint64 m_layoutGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_layoutCancel;
    struct CachedLayout {
        trace::GraphLayout layout;
        QVector<QPointF> positions;
    };
    QHash<quint64, CachedLayout> m_layoutCache;
    QTimer *m_animTimer = nullptr;
    QVector<QPointF> m_animFrom;
    QVector<QPointF> m_animTo;
    int m_animStep = 0;
    QHash<int, QString> m_overviewRankHost; // rank -> compute node
    double m_zoom = 1;
    double m_unitSpacing = 0.1;
    QPointF m_pan;
    int m_highlightFinding = -1;
    QVector<bool> m_highlightNodes;
    int m_highlightEdge = -1;
    QVector<int> m_highlightEdges; // links of the highlighted finding
    QString m_edgeColor = QStringLiteral("wait");
    QString m_nodeColor = QStringLiteral("wait");
    QString m_edgeFilter = QStringLiteral("all");
    double m_edgeScaleMax = 0;   // largest value of the edge colour quantity
    double m_edgeScaleMin = 0;
    double m_maxCompute = 0;
    double m_minCompute = 0;
    trace::CommGraph m_rankGraph;  // the window at rank level (findings, reference)
    trace::CommGraph m_reference;
    QString m_referenceLabel;
    QHash<quint64, double> m_referenceLinkWait; // (sender << 32 | receiver) -> waiting per second
    QVector<trace::CollectiveInstance> m_collInstances; // of the window, by first enter
    QPair<quint64, qint64> m_focusKey{0, -1};           // collective stepped to: (comm, last enter)
    int m_focusCollective = -1;                          // its index in m_collInstances
    trace::CriticalPath m_critical;
    bool m_showCritical = false;
    QVector<bool> m_criticalNodes;
    QVector<int> m_criticalEdges;
    qint64 m_navFirst = -1;    // first enter of the collective stepped to last
    qint64 m_navViewFrom = -1; // the view start set for it (a changed view resets the anchor)
    QVector<QVariantList> m_findingSeries;      // per finding: waiting over time of its ranks
    QVector<QVariantList> m_findingBaseline;    // per finding: of all ranks
    int m_maxEdges = kDefaultMaxEdges;
    trace::FindingSummary m_summary;
    int m_notableFrom = 0; // m_drawn[m_notableFrom..]: drawn strong (the rest: pale context)

    trace::CommGraph m_graph;
    QVector<QPointF> m_unitPos; // layoutGraph() result
    QVector<int> m_drawn;       // indices into m_graph.edges, heaviest first
    double m_maxWeight = 0;
    QVector<double> m_nodeWait; // waiting (late sender) per graph node, seconds
    double m_maxWait = 0;       // largest waiting of a rank / host node
    double m_minWait = 0;       // smallest waiting of a rank / host node
    QVector<trace::GraphFinding> m_findings;
    quint64 m_maxInternal = 0;  // largest traffic inside one compute node (nodes level)
    QString m_status = QStringLiteral("empty");

    bool m_requested = false;
    qint64 m_requestedFrom = 0;
    qint64 m_requestedTo = 0;
};
