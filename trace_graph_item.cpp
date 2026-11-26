#include "trace_graph_item.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSet>

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
    return m_layout == trace::GraphLayout::Grid ? QStringLiteral("grid") : QStringLiteral("circle");
}

void TraceGraphItem::setLayout(const QString &layout)
{
    const trace::GraphLayout l = layout == QLatin1String("grid") ? trace::GraphLayout::Grid : trace::GraphLayout::Circle;
    if (l == m_layout)
        return;
    m_layout = l;
    emit displayChanged();
    m_unitPos = trace::layoutGraph(m_graph, m_layout);
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

QString TraceGraphItem::view() const
{
    switch (m_viewMode) {
    case Mode::On: return QStringLiteral("matrix");
    case Mode::Off: return QStringLiteral("graph");
    case Mode::Auto: break;
    }
    return QStringLiteral("auto");
}

void TraceGraphItem::setView(const QString &view)
{
    const Mode m = view == QLatin1String("matrix") ? Mode::On : view == QLatin1String("graph") ? Mode::Off : Mode::Auto;
    if (m == m_viewMode)
        return;
    m_viewMode = m;
    emit displayChanged();
    rebuild();
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
        rankGraphFindings = trace::analyzeCommGraph(
            rankOptions.level == m_options.level && m_options.directed && m_options.expandedNodes.isEmpty()
                ? m_graph
                : trace::buildCommGraph(*detail, from, to, rankOptions));
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
    m_unitPos = trace::layoutGraph(m_graph, m_layout);
    m_findings = rankGraphFindings;
    m_matrix = m_viewMode == Mode::On || (m_viewMode == Mode::Auto && m_graph.rankNodeCount > kAutoMatrixLimit);
    m_p2pIndex.clear();
    for (int i = 0; i < m_graph.edges.size(); ++i) {
        const GraphEdge &e = m_graph.edges[i];
        if (e.kind == GraphEdge::Kind::P2P)
            m_p2pIndex.insert((quint64(quint32(e.from)) << 32) | quint32(e.to), i);
    }
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
    reselect();
    emit graphChanged();
    update();
}

void TraceGraphItem::reselect()
{
    m_drawn = trace::selectEdges(m_graph, m_weight, m_maxEdges, focusNode());
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

int TraceGraphItem::edgeBetween(int fromNode, int toNode) const
{
    return m_p2pIndex.value((quint64(quint32(fromNode)) << 32) | quint32(toNode), -1);
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
    const int n = std::max(1, m_graph.rankNodeCount);
    // Neighbours on the circle may touch at most; between 2.5 and 10 px.
    return std::clamp(kPi * radius() / n * 0.8, 2.5, 10.0);
}

QPointF TraceGraphItem::nodePosition(int node) const
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
        if (m_layout == trace::GraphLayout::Circle)
            control = mid + (center() - mid) * (0.5 * len / (2 * radius()));
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

int TraceGraphItem::edgeAt(QPointF p) const
{
    // Topmost (heaviest) first.
    for (int k = int(m_drawn.size()) - 1; k >= 0; --k) {
        const GraphEdge &e = m_graph.edges[m_drawn[k]];
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

namespace {
constexpr double kMatrixLeft = 84;   // host / rank labels of the rows
constexpr double kMatrixTop = 40;    // hint line + column labels
constexpr double kMatrixRight = 44;  // waiting strip
constexpr double kMatrixBottom = 26; // axis caption
constexpr double kWaitStrip = 14;
} // namespace

QRectF TraceGraphItem::matrixRect() const
{
    const int n = m_graph.rankNodeCount;
    if (n <= 0)
        return {};
    const double side = std::max(10.0, std::min(width() - kMatrixLeft - kMatrixRight, height() - kMatrixTop - kMatrixBottom));
    const double left = kMatrixLeft + std::max(0.0, (width() - kMatrixLeft - kMatrixRight - side) / 2);
    return QRectF(left, kMatrixTop, side, side);
}

QPoint TraceGraphItem::matrixCellAt(QPointF p) const
{
    const QRectF r = matrixRect();
    const int n = m_graph.rankNodeCount;
    if (!m_matrix || n <= 0 || !r.contains(p))
        return QPoint(-1, -1);
    const int col = std::clamp(int((p.x() - r.left()) / r.width() * n), 0, n - 1);
    const int row = std::clamp(int((p.y() - r.top()) / r.height() * n), 0, n - 1);
    return QPoint(col, row);
}

QPointF TraceGraphItem::matrixCellCenter(int fromNode, int toNode) const
{
    const QRectF r = matrixRect();
    const int n = std::max(1, m_graph.rankNodeCount);
    return QPointF(r.left() + (toNode + 0.5) * r.width() / n, r.top() + (fromNode + 0.5) * r.height() / n);
}

void TraceGraphItem::paintMatrix(QPainter *painter)
{
    const int n = m_graph.rankNodeCount;
    const QRectF r = matrixRect();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(QPen(QColor(0xd1, 0xd5, 0xdb), 1));
    painter->setBrush(QColor(0xfa, 0xfa, 0xfa));
    painter->drawRect(r);

    // Cells: one image pixel per screen pixel; several cells on one pixel keep the largest value
    // (a thin band of a stencil must not vanish when 1024 ranks share 800 pixels).
    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const int px = std::max(1, int(std::floor(r.width() * dpr)));
    const bool waitScale = m_weight == trace::GraphWeight::Wait;
    double maxW = 0;
    for (const GraphEdge &e : std::as_const(m_graph.edges))
        if (e.kind == GraphEdge::Kind::P2P)
            maxW = std::max(maxW, trace::edgeWeight(e, m_weight));
    // Inside a compute node (nodes level): the diagonal.
    if (!waitScale)
        for (int i = 0; i < n; ++i)
            maxW = std::max(maxW, m_weight == trace::GraphWeight::Bytes ? double(m_graph.nodes[i].internalBytes)
                                                                        : double(m_graph.nodes[i].internalMessages));
    QVector<float> cells(px * px, -1.0f);
    auto put = [&](int row, int col, double w) {
        const int x0 = int(double(col) * px / n), x1 = std::max(x0 + 1, int(double(col + 1) * px / n));
        const int y0 = int(double(row) * px / n), y1 = std::max(y0 + 1, int(double(row + 1) * px / n));
        // Log scale: bytes and waiting span several orders of magnitude.
        const float t = maxW > 0 ? float(std::log1p(w / maxW * 1000) / std::log1p(1000.0)) : 0.0f;
        for (int y = y0; y < std::min(y1, px); ++y)
            for (int x = x0; x < std::min(x1, px); ++x)
                cells[y * px + x] = std::max(cells[y * px + x], t);
    };
    for (const GraphEdge &e : std::as_const(m_graph.edges))
        if (e.kind == GraphEdge::Kind::P2P && e.from < n && e.to < n)
            put(e.from, e.to, trace::edgeWeight(e, m_weight));
    if (!waitScale)
        for (int i = 0; i < n; ++i) {
            const GraphNode &g = m_graph.nodes[i];
            const double w = m_weight == trace::GraphWeight::Bytes ? double(g.internalBytes) : double(g.internalMessages);
            if (w > 0)
                put(i, i, w);
        }
    // Colours: messages / bytes light to dark green (P2P colour), waiting light to dark blue.
    const QColor lo = waitScale ? QColor(0xdb, 0xe4, 0xff) : QColor(0xcf, 0xf5, 0xcf);
    const QColor hi = waitScale ? QColor(0x00, 0x00, 0x8b) : QColor(0x00, 0x4d, 0x00);
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int y = 0; y < px; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < px; ++x) {
            const float t = cells[y * px + x];
            if (t >= 0)
                line[x] = mix(lo, hi, t).rgba();
        }
    }
    img.setDevicePixelRatio(dpr);
    painter->drawImage(r.topLeft(), img);

    // Compute node borders (ranks of several hosts) and their names.
    QFont f = painter->font();
    f.setPointSizeF(8);
    painter->setFont(f);
    const QFontMetricsF fm(f);
    const double cell = r.width() / n;
    if (m_graph.groups.size() > 1 && m_options.level == trace::GraphLevel::Ranks) {
        int first = 0;
        double lastLabelY = -1e9;
        while (first < n) {
            const int g = m_graph.nodes[first].group;
            int last = first;
            while (last + 1 < n && m_graph.nodes[last + 1].group == g)
                ++last;
            if (first > 0) {
                QColor line(0x9c, 0xa3, 0xaf);
                line.setAlphaF(0.6);
                painter->setPen(QPen(line, 1));
                painter->drawLine(QPointF(r.left(), r.top() + first * cell), QPointF(r.right(), r.top() + first * cell));
                painter->drawLine(QPointF(r.left() + first * cell, r.top()), QPointF(r.left() + first * cell, r.bottom()));
            }
            const double y = r.top() + (first + last + 1) / 2.0 * cell;
            if (y - lastLabelY >= fm.height()) {
                painter->setPen(kTextColor);
                painter->drawText(QRectF(r.left() - kMatrixLeft + 2, y - 8, kMatrixLeft - 8, 16),
                                  Qt::AlignRight | Qt::AlignVCenter, m_graph.groups.value(g));
                lastLabelY = y;
            }
            first = last + 1;
        }
    } else {
        // Row labels: ranks / hosts when there is room.
        double lastY = -1e9;
        for (int i = 0; i < n; ++i) {
            const double y = r.top() + (i + 0.5) * cell;
            if (y - lastY < fm.height())
                continue;
            painter->setPen(kTextColor);
            painter->drawText(QRectF(r.left() - kMatrixLeft + 2, y - 8, kMatrixLeft - 8, 16),
                              Qt::AlignRight | Qt::AlignVCenter, nodeLabel(i));
            lastY = y;
        }
    }

    // Selected rank: its row (what it sends) and column (what it receives).
    const int focus = focusNode();
    if (focus >= 0 && focus < n) {
        QColor sel = kSelectionColor;
        sel.setAlphaF(0.18);
        painter->fillRect(QRectF(r.left(), r.top() + focus * cell, r.width(), std::max(1.0, cell)), sel);
        painter->fillRect(QRectF(r.left() + focus * cell, r.top(), std::max(1.0, cell), r.height()), sel);
    }

    // Waiting per rank / host: strip right of the matrix (white = least, blue = most waiting).
    const QRectF strip(r.right() + 8, r.top(), kWaitStrip, r.height());
    for (int i = 0; i < n; ++i)
        painter->fillRect(QRectF(strip.left(), r.top() + i * cell, kWaitStrip, std::max(cell, 1.0 / dpr)),
                          waitColor(m_nodeWait.value(i)));
    painter->setPen(QPen(QColor(0x9c, 0xa3, 0xaf), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(strip);

    painter->setPen(kTextColor);
    painter->drawText(QRectF(r.left(), kMatrixTop - 18, r.width(), 16), Qt::AlignCenter,
                      tr("receiver →"));
    painter->drawText(QRectF(r.left(), r.bottom() + 4, r.width(), 16), Qt::AlignCenter,
                      tr("rows: sender · columns: receiver · colour: %1 (log scale)")
                          .arg(m_weight == trace::GraphWeight::Wait ? tr("waiting time")
                               : m_weight == trace::GraphWeight::Messages ? tr("messages") : tr("bytes")));
    painter->save();
    painter->translate(strip.right() + 14, r.center().y());
    painter->rotate(90);
    painter->drawText(QRectF(-r.height() / 2, -8, r.height(), 16), Qt::AlignCenter, tr("waiting per row"));
    painter->restore();
}

void TraceGraphItem::clickAt(qreal x, qreal y)
{
    const QPointF p(x, y);
    if (m_matrix) {
        const QPoint c = matrixCellAt(p);
        if (c.x() < 0)
            return;
        const int edge = edgeBetween(c.y(), c.x());
        const GraphNode &sender = m_graph.nodes[c.y()];
        if (edge < 0) {
            if (sender.kind == GraphNode::Kind::Rank)
                setSelectedRank(sender.rank == m_selectedRank ? -1 : sender.rank);
            return;
        }
        if (sender.kind == GraphNode::Kind::Rank)
            setSelectedRank(sender.rank);
        // Then the edge details, as for a clicked line (below).
    }
    const int node = m_matrix ? -1 : nodeAt(p);
    if (node >= 0) {
        const GraphNode &n = m_graph.nodes[node];
        if (n.kind == GraphNode::Kind::Rank)
            setSelectedRank(n.rank == m_selectedRank ? -1 : n.rank);
        return;
    }
    const QPoint cell = m_matrix ? matrixCellAt(p) : QPoint(-1, -1);
    const int edge = m_matrix ? (cell.x() >= 0 ? edgeBetween(cell.y(), cell.x()) : -1) : edgeAt(p);
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
    const int node = m_matrix ? matrixCellAt(QPointF(x, y)).y() : nodeAt(QPointF(x, y));
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
    if (m_matrix) {
        const QPoint c = matrixCellAt(p);
        if (c.x() < 0) {
            // Waiting strip: the row's waiting.
            const QRectF r = matrixRect();
            const int n = m_graph.rankNodeCount;
            if (n > 0 && p.x() >= r.right() + 8 && p.x() <= r.right() + 8 + kWaitStrip && p.y() >= r.top() && p.y() < r.bottom()) {
                const int row = std::clamp(int((p.y() - r.top()) / r.height() * n), 0, n - 1);
                const GraphNode &rn = m_graph.nodes[row];
                const QString who = rn.kind == GraphNode::Kind::Rank ? tr("Rank %1 (%2)").arg(rn.rank).arg(rn.node) : nodeLabel(row);
                return tr("%1: waiting %2").arg(who, formatMs(nodeWaitSeconds(row)));
            }
            return {};
        }
        auto name = [&](int i) {
            const GraphNode &n = m_graph.nodes[i];
            return n.kind == GraphNode::Kind::Rank ? tr("rank %1 (%2)").arg(n.rank).arg(n.node) : nodeLabel(i);
        };
        if (c.x() == c.y() && m_graph.nodes[c.x()].kind == GraphNode::Kind::Node) {
            const GraphNode &n = m_graph.nodes[c.x()];
            return tr("Inside compute node %1: %2 in %3 messages").arg(n.node)
                .arg(formatBytes(double(n.internalBytes))).arg(n.internalMessages);
        }
        const int e = edgeBetween(c.y(), c.x());
        QStringList lines;
        lines << tr("%1 → %2").arg(name(c.y()), name(c.x()));
        if (e < 0) {
            lines << tr("no messages in this window");
        } else {
            const GraphEdge &ed = m_graph.edges[e];
            lines << tr("%1 messages, %2").arg(ed.messages).arg(formatBytes(double(ed.bytes)))
                  << tr("Receiver waiting (late sender): %1").arg(formatMs(ed.waitSeconds))
                  << tr("Click: select the sender, details in Trace Options");
        }
        return lines.join(QLatin1Char('\n'));
    }
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
    if (m_matrix) {
        paintMatrix(painter);
        paintHint(painter);
        return;
    }
    painter->setRenderHint(QPainter::Antialiasing, true);

    const int focus = focusNode();
    const double nr = nodeRadius();
    const bool arrows = m_options.directed && m_drawn.size() <= kArrowHeadLimit;

    // Partners of the selected rank (drawn in full, the rest dimmed).
    QVector<bool> lit(m_graph.nodes.size(), focus < 0);
    if (focus >= 0) {
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
        const double r = radius() + nr + 6;
        const QRectF box(center().x() - r, center().y() - r, 2 * r, 2 * r);
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
                const double off = m_graph.rankNodeCount <= kLabelLimit ? 26 : 10;
                const QPointF at = center() + QPointF(std::cos(am), std::sin(am)) * (r + off);
                painter->setPen(kTextColor);
                const double tw = fm.horizontalAdvance(name) + 4;
                painter->drawText(outsideLabelRect(at, at - center(), tw), Qt::AlignCenter, name);
            }
            first = last + 1;
        }
    }

    // Edges, lightest first.
    for (int i : std::as_const(m_drawn)) {
        const GraphEdge &e = m_graph.edges[i];
        const EdgePath path = edgePath(e);
        const bool dim = focus >= 0 && e.from != focus && e.to != focus;
        QColor c = e.kind == GraphEdge::Kind::Collective ? kCollectiveColor : kP2PColor;
        c.setAlphaF(dim ? 0.12 : (e.kind == GraphEdge::Kind::Collective ? 0.6 : 0.85));
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

    // Nodes.
    QFont f = painter->font();
    f.setPointSizeF(8);
    painter->setFont(f);
    const bool rankLabels = m_options.level == trace::GraphLevel::Nodes || m_graph.rankNodeCount <= kLabelLimit;
    for (int i = 0; i < m_graph.nodes.size(); ++i) {
        const GraphNode &n = m_graph.nodes[i];
        const QPointF p = nodePosition(i);
        // Fill: waiting of the rank / host (white to blue, as in the 3D view). Border: the host
        // colour when the ranks of several hosts are shown, the selection colour when selected.
        QColor fill = n.kind == GraphNode::Kind::Communicator
            ? kCommunicatorColor
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

        const bool label = n.kind != GraphNode::Kind::Rank || rankLabels;
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
        if (n.kind != GraphNode::Kind::Communicator && m_layout == trace::GraphLayout::Circle) {
            const QPointF out = p - center();
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
        // Matrix: above it; graph: at the bottom, where it does not cover the top node's label.
        const QRectF at = m_matrix ? QRectF(0, 4, width(), 18) : QRectF(0, height() - 20, width(), 18);
        painter->drawText(at, Qt::AlignHCenter | Qt::AlignTop, hint);
    }
}
