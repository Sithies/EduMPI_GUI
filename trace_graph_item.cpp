#include "trace_graph_item.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QCoreApplication>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <thread>

#include <algorithm>
#include <cmath>

#include "trace_timeline_model.h"

using trace::CommGraph;
using trace::GraphEdge;
using trace::GraphNode;
using trace::TimelineStore;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMargin = 44;  // room for labels and node arcs around the circle

const QColor kBackground(0xff, 0xff, 0xff);
const QColor kTextColor(0x37, 0x41, 0x51);
const QColor kHintColor(0x6b, 0x72, 0x80);
// Colours of the 3D view (Cores3D.qml): P2P lines green, collective lines black, waiting
// (late sender per process) white to blue. New elements: communicator node, host colours.
const QColor kP2PColor(0x00, 0xd5, 0x00);
const QColor kCollectiveColor(0x00, 0x00, 0x00);
const QColor kWaitLowColor(0xff, 0xff, 0xff);
const QColor kWaitHighColor(0x00, 0x00, 0xff);
const QColor kCommunicatorColor(0xd1, 0xd5, 0xdb);
const QColor kSelectionColor(0x1d, 0x4e, 0xd8);
const QColor kNodeBorder(0x1f, 0x29, 0x37);

// Node (host) colours, indexed by group % 10.
const QColor kGroupPalette[10] = {
    QColor(0x4e, 0x79, 0xa7), QColor(0xf2, 0x8e, 0x2b), QColor(0x59, 0xa1, 0x4f), QColor(0xb0, 0x7a, 0xa1),
    QColor(0x76, 0xb7, 0xb2), QColor(0xed, 0xc9, 0x48), QColor(0xe1, 0x57, 0x59), QColor(0xff, 0x9d, 0xa7),
    QColor(0x9c, 0x75, 0x5f), QColor(0xba, 0xb0, 0xac),
};

QColor groupColor(int group)
{
    return group < 0 ? kCollectiveColor : kGroupPalette[group % 10];
}

// Text next to the point at, on the side of dir (a direction away from the graph's centre):
// left-aligned to the right of the centre, right-aligned to the left of it, centred above and
// below, so that long labels do not run back over their node.
QRectF outsideLabelRect(QPointF at, QPointF dir, double width)
{
    const double len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
    const double cx = len > 0 ? dir.x() / len : 0;
    // Shift by half the width the more the label points sideways (cx = ±1: fully beside).
    const double left = at.x() - width / 2 + cx * width / 2;
    return QRectF(left, at.y() - 8, width, 16);
}

QString formatBytes(double bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1024 && u < 4) {
        bytes /= 1024;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(bytes, 0, 'f', u == 0 ? 0 : 1).arg(QLatin1String(units[u]));
}

QString formatMs(double seconds)
{
    return QStringLiteral("%1 ms").arg(seconds * 1000, 0, 'f', 3);
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

bool covers(const TimelineStore *s, qint64 from, qint64 to)
{
    return s && (s->complete || (s->fromNs <= from && to <= s->toNs));
}

QPointF bezierAt(const QPointF &p0, const QPointF &c, const QPointF &p1, double t)
{
    const double u = 1 - t;
    return u * u * p0 + 2 * u * t * c + t * t * p1;
}

double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = len2 > 0 ? ((p - a).x() * ab.x() + (p - a).y() * ab.y()) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF d = p - (a + t * ab);
    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

} // namespace

TraceGraphItem::TraceGraphItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setOpaquePainting(true);
}

QObject *TraceGraphItem::model() const
{
    return m_model;
}

void TraceGraphItem::setModel(QObject *model)
{
    auto *m = qobject_cast<TraceTimelineModel *>(model);
    if (m == m_model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = m;
    m_requested = false;
    if (m_model)
        connect(m_model, &TraceTimelineModel::changed, this, &TraceGraphItem::rebuild);
    emit modelChanged();
    maybeRequestDetail();
    rebuild();
}

void TraceGraphItem::setViewStartNs(qint64 ns)
{
    if (ns == m_viewStart)
        return;
    m_viewStart = ns;
    emit viewChanged();
    maybeRequestDetail();
    rebuild();
}

void TraceGraphItem::setViewEndNs(qint64 ns)
{
    if (ns == m_viewEnd)
        return;
    m_viewEnd = ns;
    emit viewChanged();
    maybeRequestDetail();
    rebuild();
}

void TraceGraphItem::setSelectedRank(int rank)
{
    if (rank < -1)
        rank = -1;
    if (rank == m_selectedRank)
        return;
    m_selectedRank = rank;
    emit selectedRankChanged();
    reselect();
    update();
}

void TraceGraphItem::setShowP2P(bool on)
{
    if (on == m_options.p2p)
        return;
    m_options.p2p = on;
    emit displayChanged();
    rebuild();
}

void TraceGraphItem::setShowCollectives(bool on)
{
    if (on == m_options.collectives)
        return;
    m_options.collectives = on;
    emit displayChanged();
    rebuild();
}

void TraceGraphItem::setDirected(bool on)
{
    if (on == m_options.directed)
        return;
    m_options.directed = on;
    emit displayChanged();
    rebuild();
}

QString TraceGraphItem::weight() const
{
    switch (m_weight) {
    case trace::GraphWeight::Messages: return QStringLiteral("messages");
    case trace::GraphWeight::Wait: return QStringLiteral("wait");
    case trace::GraphWeight::Bytes: break;
    }
    return QStringLiteral("bytes");
}

void TraceGraphItem::setWeight(const QString &weight)
{
    const trace::GraphWeight w = weight == QLatin1String("messages") ? trace::GraphWeight::Messages
                               : weight == QLatin1String("wait")     ? trace::GraphWeight::Wait
                                                                     : trace::GraphWeight::Bytes;
    if (w == m_weight)
        return;
    m_weight = w;
    emit displayChanged();
    reselect();
    update();
}

QString TraceGraphItem::level() const
{
    switch (m_levelMode) {
    case Mode::On: return QStringLiteral("nodes");
    case Mode::Off: return QStringLiteral("ranks");
    case Mode::Auto: break;
    }
    return QStringLiteral("auto");
}

void TraceGraphItem::setLevel(const QString &level)
{
    const Mode m = level == QLatin1String("nodes") ? Mode::On : level == QLatin1String("ranks") ? Mode::Off : Mode::Auto;
    if (m == m_levelMode)
        return;
    m_levelMode = m;
    emit displayChanged();
    rebuild();
}

QString TraceGraphItem::effectiveLevel() const
{
    return m_options.level == trace::GraphLevel::Nodes ? QStringLiteral("nodes") : QStringLiteral("ranks");
}

QString TraceGraphItem::communicators() const
{
    switch (m_commMode) {
    case Mode::On: return QStringLiteral("merged");
    case Mode::Off: return QStringLiteral("separate");
    case Mode::Auto: break;
    }
    return QStringLiteral("auto");
}

void TraceGraphItem::setCommunicators(const QString &mode)
{
    const Mode m = mode == QLatin1String("merged") ? Mode::On : mode == QLatin1String("separate") ? Mode::Off : Mode::Auto;
    if (m == m_commMode)
        return;
    m_commMode = m;
    emit displayChanged();
    rebuild();
}

QString TraceGraphItem::layout() const
{
    switch (m_layoutMode) {
    case LayoutMode::Auto: return QStringLiteral("auto");
    case LayoutMode::Fixed: return trace::layoutName(m_fixedLayout);
    }
    return QStringLiteral("auto");
}

void TraceGraphItem::setLayout(const QString &layout)
{
    LayoutMode mode = LayoutMode::Fixed;
    trace::GraphLayout fixed = trace::GraphLayout::Circle;
    if (layout == QLatin1String("grid"))
        fixed = trace::GraphLayout::Grid;
    else if (layout == QLatin1String("stress"))
        fixed = trace::GraphLayout::Stress;
    else if (layout == QLatin1String("spectral"))
        fixed = trace::GraphLayout::Spectral;
    else if (layout == QLatin1String("spectral circle") || layout == QLatin1String("spectralcircle"))
        fixed = trace::GraphLayout::SpectralCircle;
    else if (layout == QLatin1String("torus"))
        fixed = trace::GraphLayout::Torus;
    else if (layout != QLatin1String("circle"))
        mode = LayoutMode::Auto;
    if (mode == m_layoutMode && (mode == LayoutMode::Auto || fixed == m_fixedLayout))
        return;
    m_layoutMode = mode;
    m_fixedLayout = fixed;
    emit displayChanged();
    relayout();
    update();
}

QString TraceGraphItem::effectiveLayout() const
{
    return trace::layoutName(m_layout);
}

void TraceGraphItem::setSynchronousLayout(bool on)
{
    m_syncLayout = on;
}

// Positions for the current graph: from the cache when the structure (backbone) and the
// choice are the same as before, else computed (in the background unless synchronous). Old
// positions stay until the new ones arrive, then move there in a short animation.
void TraceGraphItem::relayout()
{
    const int n = m_graph.rankNodeCount;
    const trace::GraphLayout fixed = m_fixedLayout;
    const bool automatic = m_layoutMode == LayoutMode::Auto;
    // Simple layouts and graphs without edges: at once.
    if ((!automatic && (fixed == trace::GraphLayout::Circle || fixed == trace::GraphLayout::Grid)) || m_graph.edges.isEmpty()
        || n == 0) {
        const trace::GraphLayout l = automatic ? trace::GraphLayout::Circle : fixed;
        setPositions(trace::layoutGraph(m_graph, l, l == trace::GraphLayout::Grid ? m_gridColumns : 0), l, false);
        return;
    }
    const quint64 key = trace::backboneSignature(m_graph) ^ (quint64(automatic ? 99 : int(fixed)) << 56)
        ^ (quint64(m_gridColumns) << 40) ^ (quint64(m_graph.nodes.size()) << 20);
    if (auto it = m_layoutCache.constFind(key); it != m_layoutCache.cend() && it->positions.size() == m_graph.nodes.size()) {
        setPositions(it->positions, it->layout, true);
        return;
    }
    // Start from the positions of the same nodes now (same graph nodes: stable pictures).
    // Only a stress layout is a good start for another one (a circle would trap it).
    QVector<QPointF> previous;
    if (m_unitPos.size() == m_graph.nodes.size() && m_layout == trace::GraphLayout::Stress && !m_animTimer)
        previous = m_unitPos;
    else if (m_unitPos.size() == m_graph.nodes.size() && m_layout == trace::GraphLayout::Stress && !m_animTimer->isActive())
        previous = m_unitPos;
    const trace::CommGraph graph = m_graph;
    const int grid = m_gridColumns;
    auto job = [graph, grid, automatic, fixed, previous](const std::atomic_bool *cancel) {
        trace::LayoutChoice c;
        const QVector<QPointF> *prev = previous.isEmpty() ? nullptr : &previous;
        if (automatic) {
            c = trace::autoLayout(graph, grid, prev, cancel);
        } else {
            c.layout = fixed;
            c.positions = trace::layoutGraph(graph, fixed, grid, prev);
        }
        return c;
    };
    const quint64 gen = ++m_layoutGeneration;
    if (m_layoutCancel)
        m_layoutCancel->store(true);
    if (m_syncLayout) {
        const trace::LayoutChoice c = job(nullptr);
        m_layoutCache.insert(key, {c.layout, c.positions});
        setPositions(c.positions, c.layout, false);
        return;
    }
    m_layoutBusy = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_layoutCancel = cancel;
    QPointer<TraceGraphItem> self(this);
    std::thread([job, cancel, self, gen, key] {
        trace::LayoutChoice c = job(cancel.get());
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(qApp, [self, gen, key, c] {
            if (!self || self->m_layoutGeneration != gen)
                return;
            self->m_layoutBusy = false;
            self->m_layoutCache.insert(key, {c.layout, c.positions});
            if (self->m_layoutCache.size() > 16)
                self->m_layoutCache.erase(self->m_layoutCache.begin());
            if (c.positions.size() == self->m_graph.nodes.size())
                self->setPositions(c.positions, c.layout, true);
        }, Qt::QueuedConnection);
    }).detach();
}

void TraceGraphItem::setPositions(const QVector<QPointF> &positions, trace::GraphLayout layout, bool animate)
{
    const bool changed = layout != m_layout;
    if (animate && m_unitPos.size() == positions.size() && m_unitPos != positions) {
        m_animFrom = m_unitPos;
        m_animTo = positions;
        m_animStep = 0;
        if (!m_animTimer) {
            m_animTimer = new QTimer(this);
            m_animTimer->setInterval(16);
            connect(m_animTimer, &QTimer::timeout, this, [this] {
                ++m_animStep;
                const double t = std::min(1.0, m_animStep / 20.0);
                const double e = t * t * (3 - 2 * t);
                for (int i = 0; i < m_unitPos.size() && i < m_animTo.size(); ++i)
                    m_unitPos[i] = m_animFrom[i] + (m_animTo[i] - m_animFrom[i]) * e;
                if (t >= 1) {
                    m_unitPos = m_animTo;
                    m_animTimer->stop();
                }
                update();
            });
        }
        m_layout = layout;
        {
            const QVector<QPointF> keep = m_unitPos;
            m_unitPos = positions;
            updateSpacing();
            m_unitPos = keep;
        }
        m_animTimer->start();
    } else {
        if (m_animTimer)
            m_animTimer->stop();
        m_unitPos = positions;
        m_layout = layout;
        updateSpacing();
    }
    if (changed)
        emit graphChanged();
    update();
}

double TraceGraphItem::nodeSpacing() const
{
    // Median distance of a rank / host to its nearest neighbour (unit positions), on screen.
    return m_unitSpacing * radius() * m_zoom;
}

void TraceGraphItem::updateSpacing()
{
    const int n = std::min<int>(m_graph.rankNodeCount, int(m_unitPos.size()));
    if (n < 2) {
        m_unitSpacing = 2;
        return;
    }
    // Nearest neighbours of up to 400 sample nodes (enough for the median).
    QVector<double> nearest;
    const int step = std::max(1, n / 400);
    for (int i = 0; i < n; i += step) {
        double best = 1e300;
        for (int j = 0; j < n; ++j) {
            if (j == i)
                continue;
            const QPointF d = m_unitPos[i] - m_unitPos[j];
            best = std::min(best, d.x() * d.x() + d.y() * d.y());
        }
        nearest.append(std::sqrt(best));
    }
    std::nth_element(nearest.begin(), nearest.begin() + nearest.size() / 2, nearest.end());
    m_unitSpacing = std::max(1e-4, nearest[nearest.size() / 2]);
}

void TraceGraphItem::zoomAt(qreal x, qreal y, qreal factor)
{
    const double z = std::clamp(m_zoom * factor, 1.0, kMaxZoom);
    if (z == m_zoom)
        return;
    // Keep the point under (x, y): p = c + pan + u * zoom.
    const QPointF p(x, y);
    const QPointF u = (p - viewCenter()) / m_zoom;
    m_zoom = z;
    m_pan = p - center() - u * m_zoom;
    if (m_zoom <= 1.0001)
        m_pan = QPointF();
    emit viewTransformChanged();
    update();
}

void TraceGraphItem::panBy(qreal dx, qreal dy)
{
    if (m_zoom <= 1.0001 && m_pan.isNull())
        return; // nothing to move at the fitted view
    m_pan += QPointF(dx, dy);
    emit viewTransformChanged();
    update();
}

void TraceGraphItem::resetView()
{
    m_zoom = 1;
    m_pan = QPointF();
    emit viewTransformChanged();
    update();
}

QVariantList TraceGraphItem::findingMaps() const
{
    QVariantList out;
    for (const trace::GraphFinding &f : m_findings) {
        QVariantMap m;
        m.insert(QStringLiteral("kind"), f.kind);
        m.insert(QStringLiteral("title"), f.title);
        m.insert(QStringLiteral("metric"), f.metric);
        m.insert(QStringLiteral("detail"), f.detail);
        m.insert(QStringLiteral("text"), f.text);
        m.insert(QStringLiteral("severity"), f.severity);
        m.insert(QStringLiteral("rank"), f.rank);
        QVariantList ranks;
        for (int r : f.ranks)
            ranks << r;
        m.insert(QStringLiteral("ranks"), ranks);
        m.insert(QStringLiteral("nodes"), f.nodes);
        m.insert(QStringLiteral("fromRank"), f.fromRank);
        m.insert(QStringLiteral("toRank"), f.toRank);
        m.insert(QStringLiteral("category"), f.category);
        m.insert(QStringLiteral("action"), f.action);
        const int idx = int(out.size());
        m.insert(QStringLiteral("series"), m_findingSeries.value(idx));
        m.insert(QStringLiteral("baseline"), m_findingBaseline.value(idx));
        out << m;
    }
    return out;
}

void TraceGraphItem::setHighlightedFinding(int index)
{
    if (index < -1 || index >= m_findings.size())
        index = -1;
    if (index == m_highlightFinding)
        return;
    m_highlightFinding = index;
    updateHighlight();
    emit highlightChanged();
    update();
}

// Graph nodes of the highlighted finding: its ranks (or the hosts holding them), its nodes,
// and the P2P edge of its link.
void TraceGraphItem::updateHighlight()
{
    m_highlightNodes = QVector<bool>(m_graph.nodes.size(), false);
    m_highlightEdge = -1;
    m_highlightEdges.clear();
    if (m_highlightFinding < 0 || m_highlightFinding >= m_findings.size())
        return;
    const trace::GraphFinding &f = m_findings[m_highlightFinding];
    QSet<int> ranks(f.ranks.cbegin(), f.ranks.cend());
    if (f.rank >= 0)
        ranks.insert(f.rank);
    QHash<int, int> nodeOfRank; // rank -> graph node (rank node or its host node)
    for (int i = 0; i < m_graph.rankNodeCount; ++i) {
        const GraphNode &n = m_graph.nodes[i];
        if (n.kind == GraphNode::Kind::Rank && ranks.contains(n.rank))
            m_highlightNodes[i] = true;
        if (n.kind == GraphNode::Kind::Node && f.nodes.contains(n.node))
            m_highlightNodes[i] = true;
        if (n.kind == GraphNode::Kind::Rank)
            nodeOfRank.insert(n.rank, i);
    }
    // Hosts level: a rank's host node stands for it.
    if (m_options.level == trace::GraphLevel::Nodes && m_overviewRankHost.size() > 0) {
        for (int r : std::as_const(ranks)) {
            const QString host = m_overviewRankHost.value(r);
            for (int i = 0; i < m_graph.rankNodeCount; ++i)
                if (m_graph.nodes[i].kind == GraphNode::Kind::Node && m_graph.nodes[i].node == host)
                    m_highlightNodes[i] = true;
        }
    }
    QVector<QPair<int, int>> links = f.links;
    if (f.fromRank >= 0 && f.toRank >= 0 && !links.contains({f.fromRank, f.toRank}))
        links.append({f.fromRank, f.toRank});
    for (const auto &l : std::as_const(links)) {
        const int a = nodeOfRank.value(l.first, -1), b = nodeOfRank.value(l.second, -1);
        for (int i = 0; i < m_graph.edges.size() && a >= 0 && b >= 0; ++i)
            if (m_graph.edges[i].kind == GraphEdge::Kind::P2P && m_graph.edges[i].from == a && m_graph.edges[i].to == b) {
                m_highlightEdges.append(i);
                m_highlightEdge = i;
            }
    }
}

void TraceGraphItem::zoomToFinding(int index)
{
    setHighlightedFinding(index);
    if (m_highlightNodes.isEmpty())
        return;
    // Bounding box of the highlighted nodes at zoom 1, then zoom / pan so it fills the view.
    QRectF box;
    bool any = false;
    for (int i = 0; i < m_highlightNodes.size(); ++i) {
        if (!m_highlightNodes[i])
            continue;
        const QPointF p = basePosition(i);
        box = any ? box.united(QRectF(p, QSizeF(0.001, 0.001))) : QRectF(p, QSizeF(0.001, 0.001));
        any = true;
    }
    if (!any) {
        resetView();
        return;
    }
    // Room around the box: a few node spacings, so the neighbours stay in view.
    const double pad = 3 * m_unitSpacing * radius();
    box.adjust(-pad, -pad, pad, pad);
    const double margin = 40;
    const double z = std::clamp(std::min((width() - 2 * margin) / std::max(box.width(), 1.0),
                                         (height() - 2 * margin) / std::max(box.height(), 1.0)),
                                1.0, 6.0);
    m_zoom = z;
    // The box centre to the middle of the view.
    m_pan = -(box.center() - center()) * m_zoom;
    emit viewTransformChanged();
    update();
}

void TraceGraphItem::setMaxEdges(int n)
{
    n = std::max(0, n);
    if (n == m_maxEdges)
        return;
    m_maxEdges = n;
    emit displayChanged();
    reselect();
    update();
}

QStringList TraceGraphItem::findings() const
{
    QStringList out;
    for (const trace::GraphFinding &f : m_findings)
        out << f.text;
    return out;
}

QVariantList TraceGraphItem::findingRanks() const
{
    QVariantList out;
    for (const trace::GraphFinding &f : m_findings)
        out << f.rank;
    return out;
}

qint64 TraceGraphItem::windowStart() const
{
    return m_viewEnd > m_viewStart ? m_viewStart : 0;
}

qint64 TraceGraphItem::windowEnd() const
{
    if (m_viewEnd > m_viewStart)
        return m_viewEnd;
    return m_model ? m_model->traceLengthNs() : 0;
}

void TraceGraphItem::setStatus(const QString &status)
{
    m_status = status;
}

void TraceGraphItem::maybeRequestDetail()
{
    if (!m_model || !m_model->windowed() || !isVisible())
        return;
    const qint64 from = windowStart();
    const qint64 to = windowEnd();
    if (to <= from)
        return;
    const qint64 max = m_model->maxDetailWindowNs();
    if (max > 0 && to - from > max)
        return;
    if (covers(m_model->detail().get(), from, to))
        return;
    if (m_requested && m_requestedFrom == from && m_requestedTo == to)
        return;
    m_requested = true;
    m_requestedFrom = from;
    m_requestedTo = to;
    m_model->requestDetail(from, to);
}

void TraceGraphItem::rebuild()
{
    const auto overview = m_model ? m_model->overview() : nullptr;
    const qint64 from = windowStart();
    const qint64 to = windowEnd();
    const auto detail = m_model ? m_model->detail() : nullptr;
    // Level: auto = ranks when zoomed in (single steps) or few ranks, else compute nodes (if
    // there are several).
    bool nodes = m_levelMode == Mode::On;
    if (m_levelMode == Mode::Auto && overview && overview->rows.size() > kAutoRankLimit
        && to - from > kAutoRanksWindowNs) {
        QSet<QString> hosts;
        for (const auto &row : overview->rows)
            hosts.insert(row.node);
        nodes = hosts.size() > 1;
    }
    m_options.level = nodes ? trace::GraphLevel::Nodes : trace::GraphLevel::Ranks;
    QString status;
    QVector<trace::GraphFinding> rankGraphFindings;
    int gridColumns = 0;
    std::shared_ptr<const TimelineStore> seriesStore;
    m_rankGraph = CommGraph();
    if (overview) {
        m_overviewRankHost.clear();
        for (const auto &row : overview->rows)
            m_overviewRankHost.insert(row.rank, row.node);
    }
    if (!overview || overview->rows.isEmpty()) {
        m_graph = CommGraph();
        status = QStringLiteral("empty");
    } else if (detail && covers(detail.get(), from, to)) {
        m_graph = trace::buildCommGraph(*detail, from, to, m_options);
        // Findings always from the ranks (pattern and outliers need them), directed.
        trace::GraphOptions rankOptions = m_options;
        rankOptions.level = trace::GraphLevel::Ranks;
        rankOptions.directed = true;
        rankOptions.expandedNodes.clear();
        const trace::CommGraph rankGraph =
            rankOptions.level == m_options.level && m_options.directed && m_options.expandedNodes.isEmpty()
                ? m_graph
                : trace::buildCommGraph(*detail, from, to, rankOptions);
        const trace::CommPattern pattern = trace::detectPattern(rankGraph);
        const int patternGrid = pattern.kind == trace::CommPattern::Kind::Stencil2D ? pattern.gridColumns : 0;
        gridColumns = m_options.level == trace::GraphLevel::Ranks ? patternGrid : 0;
        rankGraphFindings = trace::analyzeCommGraph(rankGraph, patternGrid);
        if (hasReference())
            rankGraphFindings += trace::compareCommGraphs(rankGraph, m_reference);
        m_rankGraph = rankGraph;
        seriesStore = detail;
    } else {
        // Nodes without edges, so the layout is already in place.
        m_graph = trace::buildCommGraph(*overview, 0, 0, m_options);
        const qint64 max = m_model->maxDetailWindowNs();
        if (max > 0 && to - from > max)
            status = QStringLiteral("zoom");
        else if (m_model->detailOverBudget())
            status = QStringLiteral("dense");
        else
            status = QStringLiteral("loading");
    }
    setStatus(status);
    const int comms = int(m_graph.nodes.size()) - m_graph.rankNodeCount;
    m_commMerged = comms > 1
        && (m_commMode == Mode::On || (m_commMode == Mode::Auto && comms > kAutoCommunicatorLimit));
    if (m_commMerged)
        m_graph = trace::mergeCommunicators(m_graph);
    m_findings = rankGraphFindings;
    m_gridColumns = gridColumns;
    m_findingSeries.clear();
    m_findingBaseline.clear();
    if (seriesStore)
        addTimeSeries(*seriesStore, from, to);
    // Same graph nodes as before (window moved): positions stay until the new layout is there.
    if (m_unitPos.size() != m_graph.nodes.size()) {
        m_unitPos = trace::layoutGraph(m_graph, trace::GraphLayout::Circle);
        m_layout = trace::GraphLayout::Circle;
        updateSpacing();
    }
    relayout();
    m_highlightFinding = -1;
    updateHighlight();
    // Waiting of a rank (late sender): receiver of a P2P edge, caller of a collective.
    m_nodeWait = trace::nodeWaiting(m_graph);
    m_maxInternal = 0;
    for (const GraphNode &n : std::as_const(m_graph.nodes))
        m_maxInternal = std::max(m_maxInternal, n.internalBytes);
    m_maxWait = 0;
    m_minWait = -1;
    for (int i = 0; i < m_graph.rankNodeCount; ++i) {
        m_maxWait = std::max(m_maxWait, m_nodeWait[i]);
        m_minWait = m_minWait < 0 ? m_nodeWait[i] : std::min(m_minWait, m_nodeWait[i]);
    }
    m_minWait = std::max(0.0, m_minWait);
    m_maxCompute = 0;
    m_minCompute = -1;
    for (int i = 0; i < m_graph.rankNodeCount; ++i) {
        const double c = computeSeconds(i);
        m_maxCompute = std::max(m_maxCompute, c);
        m_minCompute = m_minCompute < 0 ? c : std::min(m_minCompute, c);
    }
    m_minCompute = std::max(0.0, m_minCompute);
    reselect();
    emit graphChanged();
    update();
}

void TraceGraphItem::reselect()
{
    m_drawn = trace::selectEdges(m_graph, m_weight, m_maxEdges, focusNode());
    if (m_edgeFilter != QLatin1String("all")) {
        QVector<int> kept;
        for (int i : std::as_const(m_drawn))
            if (edgePassesFilter(m_graph.edges[i]))
                kept.append(i);
        m_drawn = kept;
    }
    updateEdgeScale();
    m_maxWeight = 0;
    for (int i : std::as_const(m_drawn))
        m_maxWeight = std::max(m_maxWeight, trace::edgeWeight(m_graph.edges[i], m_weight));
    // Lightest first, so the heavy edges end up on top.
    std::reverse(m_drawn.begin(), m_drawn.end());
    emit graphChanged();
}

QColor TraceGraphItem::waitColor(double seconds) const
{
    // White at the least waiting rank / host of the window, blue at the most.
    const double span = m_maxWait - m_minWait;
    return mix(kWaitLowColor, kWaitHighColor, span > 0 ? (seconds - m_minWait) / span : 0);
}

int TraceGraphItem::focusNode() const
{
    return m_selectedRank >= 0 ? m_graph.indexOfRank(m_selectedRank) : -1;
}

QPointF TraceGraphItem::center() const
{
    return QPointF(width() / 2, height() / 2);
}

double TraceGraphItem::radius() const
{
    return std::max(10.0, std::min(width(), height()) / 2 - kMargin);
}

double TraceGraphItem::nodeRadius() const
{
    // Neighbours may touch at most; between 2.5 and 10 px (grows with the zoom).
    return std::clamp(nodeSpacing() * 0.4, 2.5, 10.0);
}

QPointF TraceGraphItem::basePosition(int node) const
{
    if (node < 0 || node >= m_unitPos.size())
        return {};
    const QPointF u = m_unitPos[node];
    if (m_layout == trace::GraphLayout::Grid) {
        const double hw = std::max(10.0, width() / 2 - kMargin);
        const double hh = std::max(10.0, height() / 2 - kMargin);
        return center() + QPointF(u.x() * hw, u.y() * hh);
    }
    return center() + u * radius();
}

QPointF TraceGraphItem::nodePosition(int node) const
{
    if (node < 0 || node >= m_unitPos.size())
        return {};
    return viewCenter() + (basePosition(node) - center()) * m_zoom;
}

TraceGraphItem::EdgePath TraceGraphItem::edgePath(const GraphEdge &edge) const
{
    EdgePath p;
    p.from = nodePosition(edge.from);
    p.to = nodePosition(edge.to);
    const QPointF mid = (p.from + p.to) / 2;
    const QPointF d = p.to - p.from;
    const double len = std::sqrt(d.x() * d.x() + d.y() * d.y());
    // Bend: P2P edges of the circle towards the centre, the more the longer the chord (edges
    // between neighbours stay short), plus a small sideways shift so that a -> b and b -> a do not overlap.
    QPointF control = mid;
    if (edge.kind == GraphEdge::Kind::P2P) {
        if (m_layout == trace::GraphLayout::Circle || m_layout == trace::GraphLayout::SpectralCircle)
            control = mid + (viewCenter() - mid) * (0.5 * len / (2 * radius() * m_zoom));
        if (m_options.directed && len > 0) {
            const QPointF normal(-d.y() / len, d.x() / len); // left of the direction of travel
            control += normal * std::min(18.0, len * 0.12);
        }
    }
    p.control = control;
    return p;
}

QPointF TraceGraphItem::edgeMidpoint(int edge) const
{
    if (edge < 0 || edge >= m_graph.edges.size())
        return {};
    const EdgePath p = edgePath(m_graph.edges[edge]);
    return bezierAt(p.from, p.control, p.to, 0.5);
}

double TraceGraphItem::edgeWidth(const GraphEdge &edge) const
{
    if (m_maxWeight <= 0)
        return 1.0;
    const double w = trace::edgeWeight(edge, m_weight) / m_maxWeight;
    // Widest edge 6 px, but not much wider than a node when the nodes are small (many ranks).
    const double widest = std::clamp(nodeRadius() * 1.2, 2.0, 6.0);
    return 0.75 + (widest - 0.75) * std::sqrt(std::max(0.0, w));
}

QString TraceGraphItem::nodeLabel(int node) const
{
    if (node < 0 || node >= m_graph.nodes.size())
        return {};
    const GraphNode &n = m_graph.nodes[node];
    switch (n.kind) {
    case GraphNode::Kind::Rank: return QString::number(n.rank);
    case GraphNode::Kind::Node: return n.node;
    case GraphNode::Kind::Communicator:
        if (n.comm == trace::kMergedCommunicators)
            return tr("all communicators");
        return tr("comm %1").arg(n.comm);
    }
    return {};
}

int TraceGraphItem::nodeAt(QPointF p) const
{
    const double r = nodeRadius() + 3;
    for (int i = int(m_graph.nodes.size()) - 1; i >= 0; --i) {
        const QPointF d = nodePosition(i) - p;
        const double hit = m_graph.nodes[i].kind == GraphNode::Kind::Rank ? r : std::max(r, 10.0);
        if (d.x() * d.x() + d.y() * d.y() <= hit * hit)
            return i;
    }
    return -1;
}

bool TraceGraphItem::isWrapEdge(const GraphEdge &e) const
{
    // Torus layout: a link across the cut (more than half the picture apart in x or y).
    if (m_layout != trace::GraphLayout::Torus || e.kind != GraphEdge::Kind::P2P || e.from >= m_unitPos.size()
        || e.to >= m_unitPos.size())
        return false;
    const QPointF d = m_unitPos[e.to] - m_unitPos[e.from];
    return std::abs(d.x()) > 0.95 || std::abs(d.y()) > 0.95;
}

int TraceGraphItem::edgeAt(QPointF p) const
{
    // Topmost (heaviest) first.
    for (int k = int(m_drawn.size()) - 1; k >= 0; --k) {
        const GraphEdge &e = m_graph.edges[m_drawn[k]];
        if (isWrapEdge(e))
            continue;
        const EdgePath path = edgePath(e);
        const double tolerance = std::max(4.0, edgeWidth(e) / 2 + 2);
        QPointF prev = path.from;
        for (int s = 1; s <= 12; ++s) {
            const QPointF next = bezierAt(path.from, path.control, path.to, s / 12.0);
            if (distanceToSegment(p, prev, next) <= tolerance)
                return m_drawn[k];
            prev = next;
        }
    }
    return -1;
}

void TraceGraphItem::clickAt(qreal x, qreal y)
{
    const QPointF p(x, y);
    const int node = nodeAt(p);
    if (node >= 0) {
        const GraphNode &n = m_graph.nodes[node];
        if (n.kind == GraphNode::Kind::Rank)
            setSelectedRank(n.rank == m_selectedRank ? -1 : n.rank);
        return;
    }
    const int edge = edgeAt(p);
    if (edge < 0)
        return;
    const GraphEdge &e = m_graph.edges[edge];
    const GraphNode &a = m_graph.nodes[e.from];
    const GraphNode &b = m_graph.nodes[e.to];
    QVariantMap d;
    d.insert(QStringLiteral("kind"), e.kind == GraphEdge::Kind::Collective ? QStringLiteral("collective")
                                                                            : QStringLiteral("p2p"));
    d.insert(QStringLiteral("from"), a.kind == GraphNode::Kind::Rank ? tr("rank %1").arg(a.rank) : nodeLabel(e.from));
    d.insert(QStringLiteral("to"), b.kind == GraphNode::Kind::Rank ? tr("rank %1").arg(b.rank) : nodeLabel(e.to));
    d.insert(QStringLiteral("fromRank"), a.kind == GraphNode::Kind::Rank ? a.rank : -1);
    d.insert(QStringLiteral("toRank"), b.kind == GraphNode::Kind::Rank ? b.rank : -1);
    d.insert(QStringLiteral("comm"), b.kind == GraphNode::Kind::Communicator ? QVariant(qulonglong(b.comm)) : QVariant());
    d.insert(QStringLiteral("bytes"), qulonglong(e.bytes));
    d.insert(QStringLiteral("messages"), e.messages);
    d.insert(QStringLiteral("waitSeconds"), e.waitSeconds);
    d.insert(QStringLiteral("directed"), e.kind == GraphEdge::Kind::P2P && m_options.directed);
    d.insert(QStringLiteral("fromNs"), windowStart());
    d.insert(QStringLiteral("toNs"), windowEnd());
    emit edgeClicked(d);
}

bool TraceGraphItem::toggleExpandAt(qreal x, qreal y)
{
    if (m_options.level != trace::GraphLevel::Nodes)
        return false;
    const int node = nodeAt(QPointF(x, y));
    if (node < 0)
        return false;
    const GraphNode &n = m_graph.nodes[node];
    if (n.kind == GraphNode::Kind::Node)
        m_options.expandedNodes.append(n.node);
    else if (n.kind == GraphNode::Kind::Rank && m_options.expandedNodes.contains(n.node))
        m_options.expandedNodes.removeAll(n.node);
    else
        return false;
    rebuild();
    return true;
}

void TraceGraphItem::collapseAll()
{
    if (m_options.expandedNodes.isEmpty())
        return;
    m_options.expandedNodes.clear();
    rebuild();
}

QString TraceGraphItem::toolTipAt(qreal x, qreal y) const
{
    const QPointF p(x, y);
    const int node = nodeAt(p);
    if (node >= 0) {
        const GraphNode &n = m_graph.nodes[node];
        // Traffic of the node with other graph nodes (P2P edges).
        quint64 sentBytes = 0, recvBytes = 0, collBytes = 0;
        int sent = 0, recv = 0, calls = 0, partners = 0;
        for (const GraphEdge &e : m_graph.edges) {
            if (e.kind == GraphEdge::Kind::Collective) {
                if (e.from == node || e.to == node) {
                    collBytes += e.bytes;
                    calls += e.messages;
                    ++partners;
                }
                continue;
            }
            if (e.from == node) {
                sentBytes += e.bytes;
                sent += e.messages;
            }
            if (e.to == node) {
                recvBytes += e.bytes;
                recv += e.messages;
            }
        }
        QStringList lines;
        const QString traffic = tr("Sent %1 in %2 messages, received %3 in %4")
                                    .arg(formatBytes(double(sentBytes))).arg(sent)
                                    .arg(formatBytes(double(recvBytes))).arg(recv);
        switch (n.kind) {
        case GraphNode::Kind::Rank:
            lines << tr("Rank %1 (compute node %2)").arg(n.rank).arg(n.node) << traffic
                  << tr("Waiting (late sender): %1").arg(formatMs(nodeWaitSeconds(node)));
            if (m_options.level == trace::GraphLevel::Nodes)
                lines << tr("Double-click: merge %1 into one node again").arg(n.node);
            else
                lines << tr("Click: select the rank (also in the timeline)");
            break;
        case GraphNode::Kind::Node:
            lines << tr("Compute node %1 with %2 ranks").arg(n.node).arg(n.members)
                  << tr("Inside the node: %1 in %2 messages (green ring)")
                         .arg(formatBytes(double(n.internalBytes))).arg(n.internalMessages)
                  << tr("To / from other nodes: ") + traffic.left(1).toLower() + traffic.mid(1)
                  << tr("Waiting (late sender), sum of its ranks: %1").arg(formatMs(nodeWaitSeconds(node)))
                  << tr("Double-click: show its ranks");
            break;
        case GraphNode::Kind::Communicator:
            if (n.comm == trace::kMergedCommunicators)
                lines << tr("All %1 communicators merged into one node").arg(n.members);
            else
                lines << tr("Communicator %1 with %2 ranks").arg(n.comm).arg(n.members);
            lines << tr("%1 collective calls from %2 graph nodes, %3")
                         .arg(calls).arg(partners).arg(formatBytes(double(collBytes)));
            break;
        }
        return lines.join(QLatin1Char('\n'));
    }
    const int edge = edgeAt(p);
    if (edge < 0)
        return {};
    const GraphEdge &e = m_graph.edges[edge];
    auto name = [&](int i) {
        const GraphNode &n = m_graph.nodes[i];
        return n.kind == GraphNode::Kind::Rank ? tr("rank %1").arg(n.rank) : nodeLabel(i);
    };
    const bool collective = e.kind == GraphEdge::Kind::Collective;
    const QString arrow = collective || !m_options.directed ? QStringLiteral(" – ") : QStringLiteral(" → ");
    QStringList lines;
    lines << (collective ? tr("Collectives: ") : tr("Messages: ")) + name(e.from) + arrow + name(e.to)
          << tr("%1 %2, %3").arg(e.messages).arg(collective ? tr("calls") : tr("messages")).arg(formatBytes(double(e.bytes)))
          << tr("Waiting (late sender): %1").arg(formatMs(e.waitSeconds))
          << tr("Click: details in Trace Options");
    return lines.join(QLatin1Char('\n'));
}

void TraceGraphItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

void TraceGraphItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue) {
        // While hidden another view may have been loaded: ask again for this window.
        m_requested = false;
        maybeRequestDetail();
        rebuild();
    }
}

void TraceGraphItem::paint(QPainter *painter)
{
    painter->fillRect(QRectF(0, 0, width(), height()), kBackground);
    if (m_graph.nodes.isEmpty())
        return;
    painter->setRenderHint(QPainter::Antialiasing, true);

    const int focus = focusNode();
    const double nr = nodeRadius();
    const bool arrows = m_options.directed && m_drawn.size() <= kArrowHeadLimit;

    // Partners of the selected rank (drawn in full, the rest dimmed); a highlighted finding
    // takes precedence: its ranks / nodes lit, the rest dimmed.
    // Only findings that point at ranks / nodes dim the rest (a structure finding without any does not).
    const bool highlight = m_highlightFinding >= 0 && m_highlightNodes.size() == m_graph.nodes.size()
        && m_highlightNodes.contains(true);
    QVector<bool> lit(m_graph.nodes.size(), focus < 0 && !highlight);
    if (highlight) {
        lit = m_highlightNodes;
    } else if (focus >= 0) {
        lit[focus] = true;
        for (int i : std::as_const(m_drawn)) {
            const GraphEdge &e = m_graph.edges[i];
            if (e.from == focus || e.to == focus) {
                lit[e.from] = true;
                lit[e.to] = true;
            }
        }
    }

    // Node (host) arcs around the circle, with the host name when there is room.
    if (m_layout == trace::GraphLayout::Circle && m_graph.rankNodeCount > 0
        && m_options.level == trace::GraphLevel::Ranks && m_graph.groups.size() > 1) {
        const double r = radius() * m_zoom + nr + 6;
        const QPointF vc = viewCenter();
        const QRectF box(vc.x() - r, vc.y() - r, 2 * r, 2 * r);
        const int n = m_graph.rankNodeCount;
        int first = 0;
        QFont f = painter->font();
        f.setPointSizeF(8);
        painter->setFont(f);
        const QFontMetricsF fm(f);
        while (first < n) {
            const int g = m_graph.nodes[first].group;
            int last = first;
            while (last + 1 < n && m_graph.nodes[last + 1].group == g)
                ++last;
            // Angles of layoutGraph: -90° + 360° * i / n, clockwise on screen. Qt arcs count
            // counter-clockwise in 1/16°, so screen angle a is Qt angle -a.
            const double step = 360.0 / n;
            const double a0 = -90 + step * (first - 0.35);
            const double a1 = -90 + step * (last + 0.35);
            QPen pen(groupColor(g), 3);
            pen.setCapStyle(Qt::FlatCap);
            painter->setPen(pen);
            painter->drawArc(box, int(-a0 * 16), int(-(a1 - a0) * 16));
            const double arcLen = (a1 - a0) / 180 * kPi * r;
            const QString name = m_graph.groups.value(g);
            if (fm.horizontalAdvance(name) < arcLen) {
                // Outside the rank labels when those are drawn.
                const double am = (a0 + a1) / 2 * kPi / 180;
                const double off = nodeSpacing() >= kLabelSpacing ? 26 : 10;
                const QPointF at = vc + QPointF(std::cos(am), std::sin(am)) * (r + off);
                painter->setPen(kTextColor);
                const double tw = fm.horizontalAdvance(name) + 4;
                painter->drawText(outsideLabelRect(at, at - vc, tw), Qt::AlignCenter, name);
            }
            first = last + 1;
        }
    }

    // Edges, lightest first.
    for (int i : std::as_const(m_drawn)) {
        const GraphEdge &e = m_graph.edges[i];
        const EdgePath path = edgePath(e);
        const bool dim = highlight ? !(lit[e.from] && lit[e.to]) : (focus >= 0 && e.from != focus && e.to != focus);
        if (isWrapEdge(e)) {
            // Across the torus cut: a short stub at each end, pointing out of the picture.
            const QPointF du = m_unitPos[e.to] - m_unitPos[e.from];
            const QPointF dir(std::abs(du.x()) > 0.95 ? (du.x() > 0 ? -1.0 : 1.0) : 0.0,
                              std::abs(du.y()) > 0.95 ? (du.y() > 0 ? -1.0 : 1.0) : 0.0);
            const double len = std::clamp(nodeSpacing() * 0.6, 4.0, 18.0);
            QColor c = edgeColorOf(i);
            c.setAlphaF(dim ? 0.12 : 0.7);
            QPen pen(c, std::max(1.0, edgeWidth(e) * 0.7));
            pen.setStyle(Qt::DotLine);
            painter->setPen(pen);
            painter->drawLine(path.from, path.from + dir * len);
            painter->drawLine(path.to, path.to - dir * len);
            continue;
        }
        QColor c = edgeColorOf(i);
        // Inside one compute node lighter, between nodes strong.
        const bool inside = e.kind == GraphEdge::Kind::P2P && m_graph.groups.size() > 1
            && m_graph.nodes[e.from].kind == GraphNode::Kind::Rank && m_graph.nodes[e.to].kind == GraphNode::Kind::Rank
            && m_graph.nodes[e.from].group == m_graph.nodes[e.to].group;
        c.setAlphaF(dim ? 0.12 : (e.kind == GraphEdge::Kind::Collective ? 0.5 : inside ? 0.45 : 0.95));
        QPen pen(c, edgeWidth(e));
        pen.setCapStyle(Qt::RoundCap);
        if (e.kind == GraphEdge::Kind::Collective)
            pen.setStyle(Qt::DashLine);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        QPainterPath pp(path.from);
        pp.quadTo(path.control, path.to);
        painter->drawPath(pp);

        if (arrows && e.kind == GraphEdge::Kind::P2P) {
            // Head at the border of the target node, along the curve's end tangent.
            QPointF dir = path.to - path.control;
            const double len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
            if (len > 0) {
                dir /= len;
                const QPointF tip = path.to - dir * (nr + 1);
                const double size = 5 + pen.widthF();
                const QPointF normal(-dir.y(), dir.x());
                QPolygonF head;
                head << tip << tip - dir * size + normal * size * 0.5 << tip - dir * size - normal * size * 0.5;
                painter->setPen(Qt::NoPen);
                painter->setBrush(c);
                painter->drawPolygon(head);
            }
        }
    }

    // Edges among the highlighted ranks also when they are not among the heaviest drawn.
    if (highlight) {
        QSet<int> drawn(m_drawn.cbegin(), m_drawn.cend());
        for (int i = 0; i < m_graph.edges.size(); ++i) {
            const GraphEdge &e = m_graph.edges[i];
            if (drawn.contains(i) || e.kind != GraphEdge::Kind::P2P || !m_highlightNodes[e.from] || !m_highlightNodes[e.to])
                continue;
            const EdgePath path = edgePath(e);
            QPen pen(kP2PColor, 2);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            QPainterPath pp(path.from);
            pp.quadTo(path.control, path.to);
            painter->drawPath(pp);
        }
    }

    // The link of the highlighted finding, on top (also when not among the drawn edges).
    for (int he : std::as_const(m_highlightEdges)) {
        if (!highlight || he < 0 || he >= m_graph.edges.size())
            continue;
        const EdgePath path = edgePath(m_graph.edges[he]);
        const QColor c(0xf9, 0x73, 0x16);
        QPen pen(c, 5);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        QPainterPath pp(path.from);
        pp.quadTo(path.control, path.to);
        painter->drawPath(pp);
        QPointF dir = path.to - path.control;
        const double len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
        if (len > 0) {
            dir /= len;
            const QPointF tip = path.to - dir * (nr + 1);
            const QPointF normal(-dir.y(), dir.x());
            QPolygonF head;
            head << tip << tip - dir * 12 + normal * 6 << tip - dir * 12 - normal * 6;
            painter->setPen(Qt::NoPen);
            painter->setBrush(c);
            painter->drawPolygon(head);
        }
    }

    // Nodes.
    QFont f = painter->font();
    f.setPointSizeF(8);
    painter->setFont(f);
    // Rank labels once neighbouring nodes are far enough apart (zoom in for more).
    // Labels sit beside the node on the circle, below it elsewhere (needs more room).
    const bool circleLabels = m_layout == trace::GraphLayout::Circle || m_layout == trace::GraphLayout::SpectralCircle;
    const bool rankLabels = m_options.level == trace::GraphLevel::Nodes
        || nodeSpacing() >= (circleLabels ? kLabelSpacing : 2 * kLabelSpacing);
    const QRectF visible = QRectF(0, 0, width(), height()).adjusted(-20, -20, 20, 20);
    for (int i = 0; i < m_graph.nodes.size(); ++i) {
        const GraphNode &n = m_graph.nodes[i];
        const QPointF p = nodePosition(i);
        if (!visible.contains(p))
            continue;
        if (highlight && m_highlightNodes[i]) {
            // Halo of a highlighted rank / node.
            QColor halo(0xf9, 0x73, 0x16);
            halo.setAlphaF(0.45);
            painter->setPen(Qt::NoPen);
            painter->setBrush(halo);
            const double hr = (n.kind == GraphNode::Kind::Rank ? nr : std::max(nr, 8.0)) + 5;
            painter->drawEllipse(p, hr, hr);
        }
        // Fill: waiting of the rank / host (white to blue, as in the 3D view). Border: the host
        // colour when the ranks of several hosts are shown, the selection colour when selected.
        QColor fill = n.kind == GraphNode::Kind::Communicator
            ? kCommunicatorColor
            : m_nodeColor == QLatin1String("compute")
                ? mix(kWaitLowColor, QColor(0xff, 0x7f, 0x0e),
                      m_maxCompute > m_minCompute ? (computeSeconds(i) - m_minCompute) / (m_maxCompute - m_minCompute) : 0)
                : waitColor(m_nodeWait.value(i));
        if (!lit[i])
            fill = mix(fill, kBackground, 0.75);
        painter->setBrush(fill);
        const bool hostBorder = n.kind == GraphNode::Kind::Rank && m_graph.groups.size() > 1;
        QPen border(i == focus ? kSelectionColor : hostBorder ? groupColor(n.group) : kNodeBorder,
                    i == focus ? 2.5 : hostBorder ? 2.0 : 0.8);
        if (!lit[i])
            border.setColor(mix(border.color(), kBackground, 0.75));
        painter->setPen(border);
        if (n.kind == GraphNode::Kind::Node && n.internalBytes > 0 && m_maxInternal > 0) {
            // Traffic inside the compute node: green ring, wider the more (relative to the window's most).
            const double t = std::sqrt(double(n.internalBytes) / double(m_maxInternal));
            QColor ring = kP2PColor;
            ring.setAlphaF(lit[i] ? 0.85 : 0.2);
            const double rw = 1.5 + 4.0 * t;
            const double rr = std::max(nr, 8.0) + 2 + rw / 2;
            painter->save();
            painter->setPen(QPen(ring, rw));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(p, rr, rr);
            painter->restore();
        }
        if (n.kind == GraphNode::Kind::Communicator) {
            const double s = std::max(nr, 8.0);
            QPolygonF diamond;
            diamond << p + QPointF(0, -s) << p + QPointF(s, 0) << p + QPointF(0, s) << p + QPointF(-s, 0);
            painter->drawPolygon(diamond);
        } else {
            const double r = n.kind == GraphNode::Kind::Node ? std::max(nr, 8.0) : nr;
            painter->drawEllipse(p, r, r);
        }

        const bool label = n.kind != GraphNode::Kind::Rank || rankLabels || (highlight && m_highlightNodes[i]);
        if (!label)
            continue;
        QString text = nodeLabel(i);
        if (n.kind == GraphNode::Kind::Communicator)
            text += QStringLiteral(" (%1)").arg(n.members);
        // Rank / host labels outside the circle (beside their node), communicator labels below
        // the diamond on a white box, so that the star's edges do not run through them.
        const QFontMetricsF lfm(painter->font());
        const double tw = lfm.horizontalAdvance(text) + 4;
        QRectF box(p.x() - tw / 2, p.y() + std::max(nr, 8.0) + 2, tw, 16);
        if (n.kind != GraphNode::Kind::Communicator
            && (m_layout == trace::GraphLayout::Circle || m_layout == trace::GraphLayout::SpectralCircle)) {
            const QPointF out = p - viewCenter();
            const double len = std::sqrt(out.x() * out.x() + out.y() * out.y());
            if (len > 0)
                box = outsideLabelRect(p + out / len * (std::max(nr, 8.0) + (n.kind == GraphNode::Kind::Node ? 18 : 4)),
                                       out, tw);
        }
        if (n.kind == GraphNode::Kind::Communicator) {
            QColor bg = kBackground;
            bg.setAlphaF(0.85);
            painter->fillRect(box, bg);
        }
        painter->setPen(lit[i] ? kTextColor : mix(kTextColor, kBackground, 0.7));
        painter->drawText(box, Qt::AlignCenter, text);
    }

    paintHint(painter);
}

namespace {
const QColor kEdgeOk(0x4c, 0xaf, 0x50);
const QColor kEdgeMid(0xf5, 0xc5, 0x42);
const QColor kEdgeBad(0xd6, 0x27, 0x28);
const QColor kVolLow(0xb2, 0xdf, 0xdb);
const QColor kVolHigh(0x00, 0x69, 0x5c);
const QColor kSizeLow(0xd1, 0xc4, 0xe9);
const QColor kSizeHigh(0x45, 0x27, 0xa0);
const QColor kInside(0x66, 0xbb, 0x6a);
const QColor kBetween(0x15, 0x65, 0xc0);
const QColor kLess(0x21, 0x66, 0xac);
const QColor kMore(0xb2, 0x18, 0x2b);
const QColor kSame(0xbd, 0xbd, 0xbd);

QColor threeStop(double t, const QColor &a, const QColor &b, const QColor &c)
{
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? mix(a, b, t * 2) : mix(b, c, (t - 0.5) * 2);
}

// 0 .. 1 on a log scale between lo and hi (values spanning orders of magnitude).
double logPos(double v, double lo, double hi)
{
    if (hi <= 0 || v <= 0)
        return 0;
    lo = std::max(lo, hi * 1e-4);
    if (hi <= lo)
        return 1;
    return std::clamp(std::log(std::max(v, lo) / lo) / std::log(hi / lo), 0.0, 1.0);
}

QString shortValue(double v, const QString &kind)
{
    if (kind == QLatin1String("volume") || kind == QLatin1String("size"))
        return formatBytes(v);
    return formatMs(v);
}
} // namespace

double TraceGraphItem::computeSeconds(int node) const
{
    if (node < 0 || node >= m_graph.rankNodeCount || m_graph.windowNs <= 0)
        return 0;
    const GraphNode &n = m_graph.nodes[node];
    const double win = m_graph.windowNs / 1e9;
    return std::max(0.0, win - n.mpiSeconds / std::max(1, n.rankCount));
}

bool TraceGraphItem::edgePassesFilter(const GraphEdge &e) const
{
    if (m_edgeFilter == QLatin1String("all"))
        return true;
    if (e.kind != GraphEdge::Kind::P2P)
        return false;
    if (m_edgeFilter == QLatin1String("waiting"))
        return e.waitSeconds > 0;
    const GraphNode &a = m_graph.nodes[e.from];
    const GraphNode &b = m_graph.nodes[e.to];
    const bool inside = a.group == b.group && a.kind == GraphNode::Kind::Rank && b.kind == GraphNode::Kind::Rank;
    if (m_edgeFilter == QLatin1String("inside"))
        return inside;
    if (m_edgeFilter == QLatin1String("between"))
        return !inside;
    return true;
}

namespace {
double changeOf(const trace::CommGraph &g, const trace::GraphEdge &e, const QHash<quint64, double> &ref)
{
    if (e.kind != trace::GraphEdge::Kind::P2P || g.nodes[e.from].kind != trace::GraphNode::Kind::Rank
        || g.nodes[e.to].kind != trace::GraphNode::Kind::Rank)
        return 0;
    const double s = g.windowNs > 0 ? g.windowNs / 1e9 : 1;
    return e.waitSeconds / s - ref.value((quint64(quint32(g.nodes[e.from].rank)) << 32) | quint32(g.nodes[e.to].rank));
}
} // namespace

void TraceGraphItem::updateEdgeScale()
{
    m_edgeScaleMax = 0;
    m_edgeScaleMin = -1;
    for (const GraphEdge &e : std::as_const(m_graph.edges)) {
        if (e.kind != GraphEdge::Kind::P2P || !edgePassesFilter(e))
            continue;
        double v = 0;
        if (m_edgeColor == QLatin1String("volume"))
            v = double(e.bytes);
        else if (m_edgeColor == QLatin1String("size"))
            v = e.messages > 0 ? double(e.bytes) / e.messages : 0;
        else if (m_edgeColor == QLatin1String("change"))
            v = std::abs(changeOf(m_graph, e, m_referenceLinkWait));
        else
            v = e.waitSeconds;
        m_edgeScaleMax = std::max(m_edgeScaleMax, v);
        if (v > 0)
            m_edgeScaleMin = m_edgeScaleMin < 0 ? v : std::min(m_edgeScaleMin, v);
    }
    m_edgeScaleMin = std::max(0.0, m_edgeScaleMin);
}

QColor TraceGraphItem::edgeColorOf(int edge) const
{
    if (edge < 0 || edge >= m_graph.edges.size())
        return kP2PColor;
    const GraphEdge &e = m_graph.edges[edge];
    if (e.kind == GraphEdge::Kind::Collective)
        return QColor(0x55, 0x55, 0x55);
    if (m_edgeColor == QLatin1String("volume"))
        return mix(kVolLow, kVolHigh, logPos(double(e.bytes), m_edgeScaleMin, m_edgeScaleMax));
    if (m_edgeColor == QLatin1String("size"))
        return mix(kSizeLow, kSizeHigh, logPos(e.messages > 0 ? double(e.bytes) / e.messages : 0, m_edgeScaleMin, m_edgeScaleMax));
    if (m_edgeColor == QLatin1String("locality")) {
        const GraphNode &a = m_graph.nodes[e.from];
        const GraphNode &b = m_graph.nodes[e.to];
        return a.group == b.group && a.kind == GraphNode::Kind::Rank && b.kind == GraphNode::Kind::Rank ? kInside : kBetween;
    }
    if (m_edgeColor == QLatin1String("change")) {
        const double d = changeOf(m_graph, e, m_referenceLinkWait);
        if (m_edgeScaleMax <= 0 || std::abs(d) < 1e-12)
            return kSame;
        const double t = std::clamp(std::sqrt(std::abs(d) / m_edgeScaleMax), 0.0, 1.0);
        return mix(kSame, d > 0 ? kMore : kLess, 0.25 + 0.75 * t);
    }
    // Waiting: green (none) -> yellow -> red (the most waiting link).
    if (e.waitSeconds <= 0)
        return kEdgeOk;
    // Square root of the share of the largest: a few heavy links stand out, little waiting
    // stays green.
    return threeStop(m_edgeScaleMax > 0 ? std::sqrt(e.waitSeconds / m_edgeScaleMax) : 0, kEdgeOk, kEdgeMid, kEdgeBad);
}

QVariantMap TraceGraphItem::edgeLegend() const
{
    QVariantMap m;
    QStringList colors;
    if (m_edgeColor == QLatin1String("locality")) {
        m.insert(QStringLiteral("title"), tr("Line colour: inside / between compute nodes"));
        m.insert(QStringLiteral("categories"), QVariantList{
            QVariantMap{{QStringLiteral("color"), kInside.name()}, {QStringLiteral("label"), tr("inside a node")}},
            QVariantMap{{QStringLiteral("color"), kBetween.name()}, {QStringLiteral("label"), tr("between nodes")}}});
        return m;
    }
    QString title, lo = shortValue(m_edgeScaleMin, m_edgeColor), hi = shortValue(m_edgeScaleMax, m_edgeColor);
    if (m_edgeColor == QLatin1String("volume")) {
        title = tr("Line colour: bytes on the link");
        colors << kVolLow.name() << kVolHigh.name();
    } else if (m_edgeColor == QLatin1String("size")) {
        title = tr("Line colour: average message size");
        colors << kSizeLow.name() << kSizeHigh.name();
    } else if (m_edgeColor == QLatin1String("change")) {
        title = tr("Line colour: waiting vs. reference (per s)");
        colors << kLess.name() << kSame.name() << kMore.name();
        lo = QStringLiteral("−") + formatMs(m_edgeScaleMax);
        hi = QStringLiteral("+") + formatMs(m_edgeScaleMax);
    } else {
        title = tr("Line colour: waiting on the link (late sender)");
        colors << kEdgeOk.name() << kEdgeMid.name() << kEdgeBad.name();
        lo = tr("none");
        hi = formatMs(m_edgeScaleMax);
    }
    m.insert(QStringLiteral("title"), title);
    m.insert(QStringLiteral("colors"), colors);
    m.insert(QStringLiteral("low"), lo);
    m.insert(QStringLiteral("high"), hi);
    return m;
}

QVariantMap TraceGraphItem::nodeLegend() const
{
    QVariantMap m;
    if (m_nodeColor == QLatin1String("compute")) {
        m.insert(QStringLiteral("title"), tr("Fill: compute time (outside MPI)"));
        m.insert(QStringLiteral("colors"), QStringList{kWaitLowColor.name(), QColor(0xff, 0x7f, 0x0e).name()});
        m.insert(QStringLiteral("low"), formatMs(m_minCompute));
        m.insert(QStringLiteral("high"), formatMs(m_maxCompute));
    } else {
        m.insert(QStringLiteral("title"), m_options.level == trace::GraphLevel::Nodes ? tr("Fill: waiting per node")
                                                                                     : tr("Fill: waiting per rank"));
        m.insert(QStringLiteral("colors"), QStringList{kWaitLowColor.name(), kWaitHighColor.name()});
        m.insert(QStringLiteral("low"), formatMs(m_minWait));
        m.insert(QStringLiteral("high"), formatMs(m_maxWait));
    }
    return m;
}

void TraceGraphItem::setEdgeColor(const QString &mode)
{
    const QString m = (mode == QLatin1String("volume") || mode == QLatin1String("size") || mode == QLatin1String("locality")
                       || mode == QLatin1String("change"))
        ? mode : QStringLiteral("wait");
    if (m == m_edgeColor)
        return;
    m_edgeColor = m;
    emit displayChanged();
    updateEdgeScale();
    emit graphChanged();
    update();
}

void TraceGraphItem::setNodeColor(const QString &mode)
{
    const QString m = mode == QLatin1String("compute") ? mode : QStringLiteral("wait");
    if (m == m_nodeColor)
        return;
    m_nodeColor = m;
    emit displayChanged();
    emit graphChanged();
    update();
}

void TraceGraphItem::setEdgeFilter(const QString &filter)
{
    const QString f = (filter == QLatin1String("waiting") || filter == QLatin1String("between") || filter == QLatin1String("inside"))
        ? filter : QStringLiteral("all");
    if (f == m_edgeFilter)
        return;
    m_edgeFilter = f;
    emit displayChanged();
    reselect();
    update();
}

void TraceGraphItem::setReference()
{
    if (m_rankGraph.rankNodeCount == 0)
        return;
    m_reference = m_rankGraph;
    m_referenceLinkWait.clear();
    const double s = m_reference.windowNs > 0 ? m_reference.windowNs / 1e9 : 1;
    for (const GraphEdge &e : std::as_const(m_reference.edges))
        if (e.kind == GraphEdge::Kind::P2P && e.from < m_reference.rankNodeCount && e.to < m_reference.rankNodeCount)
            m_referenceLinkWait[(quint64(quint32(m_reference.nodes[e.from].rank)) << 32) | quint32(m_reference.nodes[e.to].rank)]
                += e.waitSeconds / s;
    m_referenceLabel = QStringLiteral("%1 – %2 s").arg(windowStart() / 1e9, 0, 'f', 6).arg(windowEnd() / 1e9, 0, 'f', 6);
    emit referenceChanged();
    rebuild();
}

void TraceGraphItem::clearReference()
{
    if (!hasReference())
        return;
    m_reference = CommGraph();
    m_referenceLinkWait.clear();
    m_referenceLabel.clear();
    if (m_edgeColor == QLatin1String("change"))
        m_edgeColor = QStringLiteral("wait");
    emit referenceChanged();
    emit displayChanged();
    rebuild();
}

bool TraceGraphItem::zoomToRank(int rank)
{
    int node = m_graph.indexOfRank(rank);
    if (node < 0 && m_overviewRankHost.contains(rank)) {
        const QString host = m_overviewRankHost.value(rank);
        for (int i = 0; i < m_graph.rankNodeCount; ++i)
            if (m_graph.nodes[i].kind == GraphNode::Kind::Node && m_graph.nodes[i].node == host)
                node = i;
    }
    if (node < 0)
        return false;
    const QPointF p = basePosition(node);
    m_zoom = std::clamp(std::max(m_zoom, 24.0 / std::max(1e-6, m_unitSpacing * radius())), 1.0, 8.0);
    m_pan = -(p - center()) * m_zoom;
    emit viewTransformChanged();
    setSelectedRank(rank);
    update();
    return true;
}

// Waiting over time (40 bins of the window) of each finding's ranks against all ranks
// (ms per rank and bin), from the activities' waiting of the detail store.
void TraceGraphItem::addTimeSeries(const TimelineStore &store, qint64 from, qint64 to)
{
    constexpr int kBins = 40;
    if (to <= from || store.rows.isEmpty())
        return;
    const double binNs = double(to - from) / kBins;
    QHash<int, QVector<double>> perRank;
    QVector<double> all(kBins, 0.0);
    for (const auto &row : store.rows) {
        QVector<double> bins(kBins, 0.0);
        auto first = std::lower_bound(row.activities.cbegin(), row.activities.cend(), from,
                                      [](const trace::Activity &a, qint64 t) { return a.startNs < t; });
        for (auto it = first; it != row.activities.cend() && it->startNs < to; ++it) {
            if (it->waitNs <= 0)
                continue;
            const int b = std::clamp(int((it->startNs - from) / binNs), 0, kBins - 1);
            bins[b] += it->waitNs / 1e6;
        }
        for (int b = 0; b < kBins; ++b)
            all[b] += bins[b] / store.rows.size();
        perRank.insert(row.rank, bins);
    }
    QVariantList baseline;
    for (double v : std::as_const(all))
        baseline << v;
    for (const trace::GraphFinding &f : std::as_const(m_findings)) {
        QVariantList series;
        QVector<int> ranks = f.ranks;
        if (ranks.isEmpty() && f.rank >= 0)
            ranks.append(f.rank);
        if (!ranks.isEmpty() && f.category != QLatin1String("structure")) {
            QVector<double> acc(kBins, 0.0);
            int used = 0;
            for (int r : std::as_const(ranks)) {
                const auto it = perRank.constFind(r);
                if (it == perRank.cend())
                    continue;
                for (int b = 0; b < kBins; ++b)
                    acc[b] += (*it)[b];
                ++used;
            }
            if (used > 0)
                for (double v : std::as_const(acc))
                    series << v / used;
        }
        m_findingSeries.append(series);
        m_findingBaseline.append(series.isEmpty() ? QVariantList() : baseline);
    }
}

void TraceGraphItem::paintHint(QPainter *painter)
{
    // Why no edges are drawn.
    QString hint;
    if (m_status == QLatin1String("zoom"))
        hint = tr("Zoom in to load the messages of this window");
    else if (m_status == QLatin1String("dense"))
        hint = tr("Too dense for the memory budget — zoom in further");
    else if (m_status == QLatin1String("loading"))
        hint = tr("Loading messages…");
    else if (m_graph.edges.isEmpty())
        hint = tr("No communication in this window");
    else if (windowEnd() - windowStart() >= kLongWindowNs)
        hint = tr("Long window: the graph sums up many steps — zoom in to see single steps");
    if (!hint.isEmpty()) {
        painter->setPen(kHintColor);
        // At the bottom, where it does not cover the top node's label.
        const QRectF at(0, height() - 20, width(), 18);
        painter->drawText(at, Qt::AlignHCenter | Qt::AlignTop, hint);
    }
}
