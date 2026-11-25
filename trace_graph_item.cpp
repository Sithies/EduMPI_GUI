#include "trace_graph_item.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

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
    return m_options.level == trace::GraphLevel::Nodes ? QStringLiteral("nodes") : QStringLiteral("ranks");
}

void TraceGraphItem::setLevel(const QString &level)
{
    const trace::GraphLevel l = level == QLatin1String("nodes") ? trace::GraphLevel::Nodes : trace::GraphLevel::Ranks;
    if (l == m_options.level)
        return;
    m_options.level = l;
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
    QString status;
    if (!overview || overview->rows.isEmpty()) {
        m_graph = CommGraph();
        status = QStringLiteral("empty");
    } else if (detail && covers(detail.get(), from, to)) {
        m_graph = trace::buildCommGraph(*detail, from, to, m_options);
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
    m_unitPos = trace::layoutGraph(m_graph, m_layout);
    // Waiting of a rank (late sender): receiver of a P2P edge, caller of a collective.
    m_nodeWait = QVector<double>(m_graph.nodes.size(), 0.0);
    for (const GraphEdge &e : std::as_const(m_graph.edges))
        m_nodeWait[e.kind == GraphEdge::Kind::P2P ? e.to : e.from] += e.waitSeconds;
    m_maxWait = 0;
    for (int i = 0; i < m_graph.nodes.size(); ++i)
        if (m_graph.nodes[i].kind != GraphNode::Kind::Communicator)
            m_maxWait = std::max(m_maxWait, m_nodeWait[i]);
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
    case GraphNode::Kind::Communicator: return tr("comm %1").arg(n.comm);
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
                const QRectF t(at.x() - 60, at.y() - 8, 120, 16);
                painter->drawText(t, Qt::AlignCenter, name);
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
            : mix(kWaitLowColor, kWaitHighColor, m_maxWait > 0 ? m_nodeWait.value(i) / m_maxWait : 0);
        if (!lit[i])
            fill = mix(fill, kBackground, 0.75);
        painter->setBrush(fill);
        const bool hostBorder = n.kind == GraphNode::Kind::Rank && m_graph.groups.size() > 1;
        QPen border(i == focus ? kSelectionColor : hostBorder ? groupColor(n.group) : kNodeBorder,
                    i == focus ? 2.5 : hostBorder ? 2.0 : 0.8);
        if (!lit[i])
            border.setColor(mix(border.color(), kBackground, 0.75));
        painter->setPen(border);
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
        // Rank / host labels outside the circle, communicator labels below the diamond.
        QPointF at = p + QPointF(0, std::max(nr, 8.0) + 8);
        if (n.kind != GraphNode::Kind::Communicator && m_layout == trace::GraphLayout::Circle) {
            QPointF out = p - center();
            const double len = std::sqrt(out.x() * out.x() + out.y() * out.y());
            if (len > 0)
                at = p + out / len * (std::max(nr, 8.0) + 9);
        }
        painter->setPen(lit[i] ? kTextColor : mix(kTextColor, kBackground, 0.7));
        painter->drawText(QRectF(at.x() - 50, at.y() - 8, 100, 16), Qt::AlignCenter, text);
    }

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
    if (!hint.isEmpty()) {
        painter->setPen(kHintColor);
        painter->drawText(QRectF(0, 4, width(), 18), Qt::AlignHCenter | Qt::AlignTop, hint);
    }
}
