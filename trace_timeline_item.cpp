#include "trace_timeline_item.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

#include "timeline_view_math.h"
#include "trace_timeline_model.h"

using trace::Category;
using trace::TimelineStore;

namespace {

const QColor kComputeColor(0xc9, 0xce, 0xd3);
const QColor kSendColor(0x4c, 0xaf, 0x50);
const QColor kRecvColor(0xd9, 0x53, 0x4f);
const QColor kCollectiveColor(0x3b, 0x82, 0xf6);
const QColor kOtherColor(0x6b, 0x72, 0x80);
const QColor kWaitColor(0xe0, 0xb8, 0x4a);
const QColor kWaitHatch(0xa0, 0x7a, 0x10);
const QColor kAxisBackground(0xf3, 0xf4, 0xf6);
const QColor kHeaderBackground(0xe5, 0xe7, 0xeb);
const QColor kTextColor(0x37, 0x41, 0x51);
const QColor kArrowColor(0x11, 0x18, 0x27);
const QColor kSelectionColor(0x1d, 0x4e, 0xd8);

// Fixed pastel palette of the phase band, indexed by qHash(function name) % 12.
const QColor kPhasePalette[12] = {
    QColor(0xfb, 0xb4, 0xae), QColor(0xb3, 0xcd, 0xe3), QColor(0xcc, 0xeb, 0xc5), QColor(0xde, 0xcb, 0xe4),
    QColor(0xfe, 0xd9, 0xa6), QColor(0xff, 0xff, 0xcc), QColor(0xe5, 0xd8, 0xbd), QColor(0xfd, 0xda, 0xec),
    QColor(0xb3, 0xe2, 0xcd), QColor(0xfd, 0xcd, 0xac), QColor(0xcb, 0xd5, 0xe8), QColor(0xf4, 0xca, 0xe4),
};

QColor categoryColor(Category c)
{
    switch (c) {
    case Category::Send: return kSendColor;
    case Category::Recv: return kRecvColor;
    case Category::Collective: return kCollectiveColor;
    case Category::OtherMpi: return kOtherColor;
    case Category::Wait: return kWaitColor;
    case Category::Compute: break;
    }
    return kComputeColor;
}

bool covers(const TimelineStore *s, qint64 from, qint64 to)
{
    return s && (s->complete || (s->fromNs <= from && to <= s->toNs));
}

} // namespace

TraceTimelineItem::TraceTimelineItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    setOpaquePainting(true);
}

QObject *TraceTimelineItem::model() const
{
    return m_model;
}

void TraceTimelineItem::setModel(QObject *model)
{
    auto *m = qobject_cast<TraceTimelineModel *>(model);
    if (m == m_model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = m;
    m_requested = false;
    if (m_model)
        connect(m_model, &TraceTimelineModel::changed, this, &TraceTimelineItem::onModelChanged);
    emit modelChanged();
    onModelChanged();
    viewRangeChanged();
}

qint64 TraceTimelineItem::traceLength() const
{
    return m_model ? m_model->traceLengthNs() : 0;
}

double TraceTimelineItem::plotWidth() const
{
    return std::max(0.0, width() - kLabelWidth);
}

void TraceTimelineItem::setViewStartNs(qint64 ns)
{
    if (ns == m_viewStart)
        return;
    m_viewStart = ns;
    emit viewChanged();
    viewRangeChanged();
}

void TraceTimelineItem::setViewEndNs(qint64 ns)
{
    if (ns == m_viewEnd)
        return;
    m_viewEnd = ns;
    emit viewChanged();
    viewRangeChanged();
}

void TraceTimelineItem::setView(qint64 start, qint64 end)
{
    const qint64 length = traceLength();
    if (length > 0) {
        qint64 span = std::clamp<qint64>(end - start, std::min(kMinWindowNs, length), length);
        start = std::clamp<qint64>(start, 0, length - span);
        end = start + span;
    }
    if (start == m_viewStart && end == m_viewEnd)
        return;
    m_viewStart = start;
    m_viewEnd = end;
    emit viewChanged();
    viewRangeChanged();
}

void TraceTimelineItem::setSelectedRank(int rank)
{
    if (rank < -1)
        rank = -1;
    if (rank == m_selectedRank)
        return;
    m_selectedRank = rank;
    emit selectedRankChanged();
    updateDerived();
    update();
}

void TraceTimelineItem::setAllArrows(bool on)
{
    if (on == m_allArrows)
        return;
    m_allArrows = on;
    emit displayChanged();
    updateDerived();
    update();
}

void TraceTimelineItem::setHighlightWait(bool on)
{
    if (on == m_highlightWait)
        return;
    m_highlightWait = on;
    emit displayChanged();
    update();
}

void TraceTimelineItem::setShowArrows(bool on)
{
    if (on == m_showArrows)
        return;
    m_showArrows = on;
    emit displayChanged();
    updateDerived();
    update();
}

void TraceTimelineItem::setShowPhases(bool on)
{
    if (on == m_showPhases)
        return;
    m_showPhases = on;
    emit displayChanged();
    rebuildLayout();
    update();
}

void TraceTimelineItem::setPhaseLevel(const QString &level)
{
    const bool inner = level == QLatin1String("inner");
    if (inner == m_innerPhases)
        return;
    m_innerPhases = inner;
    emit displayChanged();
    update();
}

void TraceTimelineItem::setContentY(qreal y)
{
    if (qFuzzyCompare(y + 1, m_contentY + 1))
        return;
    m_contentY = y;
    emit contentYChanged();
    update();
}

void TraceTimelineItem::onModelChanged()
{
    rebuildLayout();
    // A freshly loaded trace starts with the full run when no view was set.
    if (traceLength() > 0 && m_viewEnd <= m_viewStart) {
        m_viewStart = 0;
        m_viewEnd = traceLength();
        emit viewChanged();
        viewRangeChanged();
        return;
    }
    updateDerived(); // never requests detail: the range did not change
    update();
}

void TraceTimelineItem::rebuildLayout()
{
    m_rows.clear();
    m_headers.clear();
    m_layoutOfRank.clear();
    qreal y = 0;
    const auto overview = m_model ? m_model->overview() : nullptr;
    if (overview) {
        // Nodes in order of first appearance, ranks ascending within a node.
        QStringList nodes;
        QHash<QString, QVector<int>> byNode;
        for (int i = 0; i < overview->rows.size(); ++i) {
            const QString &node = overview->rows[i].node;
            if (!byNode.contains(node))
                nodes.append(node);
            byNode[node].append(i);
        }
        const qreal band = m_showPhases ? kPhaseBand : 0;
        for (const QString &node : nodes) {
            m_headers.append({node, y});
            y += kNodeHeader;
            for (int i : byNode.value(node)) {
                m_layoutOfRank.insert(overview->rows[i].rank, int(m_rows.size()));
                m_rows.append({i, overview->rows[i].rank, y, y + band});
                y += band + kBarRow;
            }
        }
    }
    if (!qFuzzyCompare(y + 1, m_contentHeight + 1)) {
        m_contentHeight = y;
        emit contentHeightChanged();
    }
}

void TraceTimelineItem::viewRangeChanged()
{
    maybeRequestDetail();
    updateDerived();
    update();
}

void TraceTimelineItem::maybeRequestDetail()
{
    if (!m_model || !m_model->windowed() || m_viewEnd <= m_viewStart || !isVisible())
        return;
    const qint64 max = m_model->maxDetailWindowNs();
    if (max > 0 && m_viewEnd - m_viewStart > max)
        return; // too long for the budget: the overview is drawn
    if (covers(m_model->detail().get(), m_viewStart, m_viewEnd))
        return;
    if (m_requested && m_requestedFrom == m_viewStart && m_requestedTo == m_viewEnd)
        return;
    m_requested = true;
    m_requestedFrom = m_viewStart;
    m_requestedTo = m_viewEnd;
    m_model->requestDetail(m_viewStart, m_viewEnd);
}

void TraceTimelineItem::updateDerived()
{
    m_arrows.clear();
    m_partners.clear();
    m_dimOthers = false;
    bool capped = false;
    const auto detail = m_model ? m_model->detail() : nullptr;
    if (detail && m_viewEnd > m_viewStart && covers(detail.get(), m_viewStart, m_viewEnd)) {
        if (m_selectedRank >= 0 && detail->rowIndexOfRank(m_selectedRank) >= 0) {
            m_dimOthers = true;
            m_partners.insert(m_selectedRank, true);
            const QVector<int> mine = trace::messagesInView(*detail, m_selectedRank, m_viewStart, m_viewEnd,
                                                            std::numeric_limits<int>::max());
            for (int i : mine) {
                const trace::Message &msg = detail->messages[i];
                m_partners.insert(msg.sender, true);
                m_partners.insert(msg.receiver, true);
            }
            if (m_showArrows && !m_allArrows) {
                m_arrows = mine.mid(0, kArrowCap);
                capped = mine.size() > kArrowCap;
            }
        }
        if (m_showArrows && m_allArrows) {
            m_arrows = trace::messagesInView(*detail, -1, m_viewStart, m_viewEnd, kArrowCap + 1);
            capped = m_arrows.size() > kArrowCap;
            if (capped)
                m_arrows.resize(kArrowCap);
        }
    }
    if (capped != m_arrowsCapped) {
        m_arrowsCapped = capped;
        emit arrowsCappedChanged();
    }
}

void TraceTimelineItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        maybeRequestDetail();
        update();
    }
}

void TraceTimelineItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue) {
        // While hidden another view may have been loaded: ask again for this one.
        m_requested = false;
        maybeRequestDetail();
        updateDerived();
        update();
    }
}

void TraceTimelineItem::zoomAt(qreal x, qreal factor)
{
    const qint64 length = traceLength();
    const double w = plotWidth();
    if (length <= 0 || w <= 0 || factor <= 0 || m_viewEnd <= m_viewStart)
        return;
    const double frac = std::clamp((x - kLabelWidth) / w, 0.0, 1.0);
    const double span = double(m_viewEnd - m_viewStart);
    const double anchor = m_viewStart + frac * span;
    const qint64 newSpan = std::clamp<qint64>(qint64(std::llround(span * factor)), std::min(kMinWindowNs, length),
                                              length);
    const qint64 start = qint64(std::llround(anchor - frac * double(newSpan)));
    setView(start, start + newSpan);
}

void TraceTimelineItem::panBy(qreal dx)
{
    const double w = plotWidth();
    if (w <= 0 || m_viewEnd <= m_viewStart)
        return;
    const double span = double(m_viewEnd - m_viewStart);
    const double shift = std::clamp(dx / w * span, -1e18, 1e18);
    const qint64 delta = qint64(std::llround(shift));
    setView(m_viewStart + delta, m_viewEnd + delta);
}

void TraceTimelineItem::showFullRun()
{
    if (traceLength() > 0)
        setView(0, traceLength());
}

const TraceTimelineItem::RowLayout *TraceTimelineItem::rowAtY(qreal y) const
{
    if (y < kAxisHeight)
        return nullptr;
    const qreal cy = y - kAxisHeight + m_contentY;
    auto it = std::upper_bound(m_rows.begin(), m_rows.end(), cy,
                               [](qreal v, const RowLayout &r) { return v < r.top; });
    if (it == m_rows.begin())
        return nullptr;
    --it;
    return cy < it->barTop + kBarRow ? &*it : nullptr;
}

void TraceTimelineItem::clickAt(qreal x, qreal y)
{
    const RowLayout *row = rowAtY(y);
    if (!row || x < 0 || x >= width())
        return;
    if (x < kLabelWidth) {
        setSelectedRank(m_selectedRank == row->rank ? -1 : row->rank);
        return;
    }
    const auto detail = m_model ? m_model->detail() : nullptr;
    if (!detail || m_viewEnd <= m_viewStart)
        return;
    const int rowIndex = detail->rowIndexOfRank(row->rank);
    const double frac = (x - kLabelWidth) / plotWidth();
    const qint64 ns = m_viewStart + qint64(frac * double(m_viewEnd - m_viewStart));
    const trace::HitResult hit = trace::activityAt(*detail, rowIndex, ns);
    if (!hit.valid)
        return;
    QVariantMap d;
    d.insert(QStringLiteral("function"), hit.function);
    d.insert(QStringLiteral("rank"), hit.rank);
    d.insert(QStringLiteral("startUs"), double(hit.startNs) / 1000.0);
    d.insert(QStringLiteral("durationUs"), double(hit.durationNs) / 1000.0);
    d.insert(QStringLiteral("waitUs"), double(hit.waitNs) / 1000.0);
    d.insert(QStringLiteral("partner"), hit.partner);
    d.insert(QStringLiteral("bytes"), double(hit.bytes));
    d.insert(QStringLiteral("tag"), hit.tag);
    d.insert(QStringLiteral("comm"), double(hit.comm));
    d.insert(QStringLiteral("callPath"), hit.callPath);
    d.insert(QStringLiteral("messageCount"), hit.messageCount);
    emit barClicked(d);
}

void TraceTimelineItem::paint(QPainter *painter)
{
    const int w = int(width());
    const int h = int(height());
    painter->fillRect(0, 0, w, h, Qt::white);
    const auto overview = m_model ? m_model->overview() : nullptr;
    const int columns = int(plotWidth());
    if (!overview || columns <= 0 || m_viewEnd <= m_viewStart)
        return;
    const auto detailPtr = m_model->detail();
    const TimelineStore *detail = covers(detailPtr.get(), m_viewStart, m_viewEnd) ? detailPtr.get() : nullptr;
    const TimelineStore &phaseStore = detail ? *detail : *overview;
    const trace::PhaseLevel level = m_innerPhases ? trace::PhaseLevel::Innermost : trace::PhaseLevel::Outermost;
    const double nsPerPx = double(m_viewEnd - m_viewStart) / columns;
    auto xOf = [&](qint64 ns) { return kLabelWidth + (double(ns - m_viewStart)) / nsPerPx; };

    QFont font = painter->font();
    font.setPixelSize(10);
    QFont small = font;
    small.setPixelSize(8);
    const QFontMetrics smallMetrics(small);
    const QBrush hatch(kWaitHatch, Qt::BDiagPattern);

    // Rows (below the axis, scrolled by contentY); only visible ones are sampled.
    painter->save();
    painter->setClipRect(0, kAxisHeight, w, h - kAxisHeight);
    const qreal offset = kAxisHeight - m_contentY;
    painter->setFont(font);
    for (const HeaderLayout &hd : m_headers) {
        const qreal y = hd.top + offset;
        if (y + kNodeHeader < kAxisHeight || y > h)
            continue;
        painter->fillRect(QRectF(0, y, w, kNodeHeader), kHeaderBackground);
        painter->setPen(kTextColor);
        painter->drawText(QRectF(4, y, w - 8, kNodeHeader), Qt::AlignVCenter | Qt::AlignLeft, hd.node);
    }
    for (const RowLayout &row : m_rows) {
        const qreal top = row.top + offset;
        const qreal barTop = row.barTop + offset;
        if (barTop + kBarRow < kAxisHeight || top > h)
            continue;
        const bool dim = m_dimOthers && !m_partners.contains(row.rank);
        painter->setOpacity(dim ? 0.3 : 1.0);

        // Label.
        if (row.rank == m_selectedRank)
            painter->fillRect(QRectF(0, top, kLabelWidth, barTop + kBarRow - top), kSelectionColor.lighter(180));
        painter->setPen(kTextColor);
        painter->setFont(font);
        painter->drawText(QRectF(2, barTop, kLabelWidth - 4, kBarRow), Qt::AlignVCenter | Qt::AlignLeft,
                          QStringLiteral("R%1").arg(row.rank));

        // Phase band.
        if (m_showPhases) {
            const int phaseRow = &phaseStore == overview.get() ? row.rowIndex : phaseStore.rowIndexOfRank(row.rank);
            const QVector<int> band =
                trace::samplePhaseBand(phaseStore, phaseRow, m_viewStart, m_viewEnd, columns, level);
            painter->setFont(small);
            for (int i = 0; i < band.size();) {
                int j = i + 1;
                while (j < band.size() && band[j] == band[i])
                    ++j;
                const int f = band[i];
                if (f >= 0 && f < phaseStore.functions.size()) {
                    const QString &name = phaseStore.functions[f];
                    const QRectF r(kLabelWidth + i, top, j - i, kPhaseBand);
                    painter->fillRect(r, kPhasePalette[qHash(name) % 12]);
                    if (j - i >= smallMetrics.horizontalAdvance(name) + 4) {
                        painter->setPen(kTextColor);
                        painter->drawText(r.adjusted(2, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, name);
                    }
                }
                i = j;
            }
        }

        // Bar: runs of equal category; wait hatched; columns with some wait get an ochre tick.
        const QVector<trace::ColumnSample> s =
            trace::sampleRow(*overview, detail, row.rowIndex, m_viewStart, m_viewEnd, columns);
        const qreal by = barTop + 1;
        const qreal bh = kBarRow - 2;
        for (int i = 0; i < s.size();) {
            int j = i + 1;
            while (j < s.size() && s[j].category == s[i].category)
                ++j;
            const Category c = s[i].category;
            const bool asWait = c == Category::Wait && m_highlightWait;
            const QRectF r(kLabelWidth + i, by, j - i, bh);
            painter->fillRect(r, asWait ? kWaitColor : categoryColor(c == Category::Wait ? Category::Recv : c));
            if (asWait)
                painter->fillRect(r, hatch);
            i = j;
        }
        if (m_highlightWait) {
            for (int i = 0; i < s.size(); ++i)
                if (s[i].hasWait && s[i].category != Category::Wait)
                    painter->fillRect(QRectF(kLabelWidth + i, by + bh - 3, 1, 3), kWaitColor);
        }
    }
    painter->setOpacity(1.0);

    // Arrows: send enter on the sender row -> receive completion on the receiver row.
    if (detail && !m_arrows.isEmpty()) {
        painter->setClipRect(kLabelWidth, kAxisHeight, w - kLabelWidth, h - kAxisHeight);
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPen pen(kArrowColor, 1);
        painter->setPen(pen);
        painter->setBrush(kArrowColor);
        for (int idx : m_arrows) {
            const trace::Message &m = detail->messages[idx];
            const int a = m_layoutOfRank.value(m.sender, -1);
            const int b = m_layoutOfRank.value(m.receiver, -1);
            if (a < 0 || b < 0)
                continue;
            const QPointF p0(xOf(m.sendNs), m_rows[a].barTop + offset + kBarRow / 2.0);
            const QPointF p1(xOf(m.recvNs), m_rows[b].barTop + offset + kBarRow / 2.0);
            if ((p0.y() < kAxisHeight && p1.y() < kAxisHeight) || (p0.y() > h && p1.y() > h))
                continue;
            painter->drawLine(p0, p1);
            const QLineF line(p1, p0);
            if (line.length() < 1)
                continue;
            const QLineF u = line.unitVector();
            const QPointF d(u.dx() * 6, u.dy() * 6);
            const QPointF n(-u.dy() * 3, u.dx() * 3);
            const QPointF head[3] = {p1, p1 + d + n, p1 + d - n};
            painter->drawPolygon(head, 3);
        }
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setBrush(Qt::NoBrush);
    }
    painter->restore();

    // Axis on top.
    painter->fillRect(0, 0, w, kAxisHeight, kAxisBackground);
    painter->setPen(kTextColor);
    painter->setFont(font);
    for (const trace::AxisTick &t : trace::axisTicks(m_viewStart, m_viewEnd, columns)) {
        const qreal x = kLabelWidth + t.x;
        painter->drawLine(QPointF(x, kAxisHeight - 5), QPointF(x, kAxisHeight));
        painter->drawText(QRectF(x + 2, 0, 120, kAxisHeight - 4), Qt::AlignVCenter | Qt::AlignLeft, t.label);
    }
    painter->drawLine(0, kAxisHeight - 1, w, kAxisHeight - 1);
}
