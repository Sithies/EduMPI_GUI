#include "trace_overview_item.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

#include "trace_timeline_model.h"

using trace::Category;
using trace::kCategoryCount;

namespace {

// Same colours as TraceTimelineItem (global constraints), index = Category.
const QColor kCategoryColors[kCategoryCount] = {
    QColor(0xc9, 0xce, 0xd3), // Compute
    QColor(0x4c, 0xaf, 0x50), // Send
    QColor(0xd9, 0x53, 0x4f), // Recv
    QColor(0x3b, 0x82, 0xf6), // Collective
    QColor(0x6b, 0x72, 0x80), // OtherMpi
    QColor(0xe0, 0xb8, 0x4a), // Wait
};
const QColor kWindowBorder(0x11, 0x18, 0x27);

} // namespace

TraceOverviewItem::TraceOverviewItem(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setOpaquePainting(true);
}

QObject *TraceOverviewItem::model() const
{
    return m_model;
}

void TraceOverviewItem::setModel(QObject *model)
{
    auto *m = qobject_cast<TraceTimelineModel *>(model);
    if (m == m_model)
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = m;
    if (m_model)
        connect(m_model, &TraceTimelineModel::changed, this, [this] { update(); });
    emit modelChanged();
    update();
}

void TraceOverviewItem::setViewStartNs(qint64 ns)
{
    if (ns == m_viewStart)
        return;
    m_viewStart = ns;
    emit viewChanged();
    update();
}

void TraceOverviewItem::setViewEndNs(qint64 ns)
{
    if (ns == m_viewEnd)
        return;
    m_viewEnd = ns;
    emit viewChanged();
    update();
}

qint64 TraceOverviewItem::traceLength() const
{
    return m_model ? m_model->traceLengthNs() : 0;
}

void TraceOverviewItem::moveWindowTo(qreal x)
{
    const qint64 length = traceLength();
    if (length <= 0 || width() <= 0)
        return;
    const qint64 span = std::clamp<qint64>(m_viewEnd - m_viewStart, 1, length);
    const double centre = std::clamp(x / width(), 0.0, 1.0) * double(length);
    const qint64 start = std::clamp<qint64>(qint64(std::llround(centre - span / 2.0)), 0, length - span);
    emit windowMoved(start, start + span);
}

void TraceOverviewItem::mousePressEvent(QMouseEvent *event)
{
    moveWindowTo(event->position().x());
    event->accept();
}

void TraceOverviewItem::mouseMoveEvent(QMouseEvent *event)
{
    moveWindowTo(event->position().x());
    event->accept();
}

void TraceOverviewItem::paint(QPainter *painter)
{
    const int w = int(width());
    const int h = int(height());
    painter->fillRect(0, 0, w, h, Qt::white);
    const auto overview = m_model ? m_model->overview() : nullptr;
    const qint64 length = traceLength();
    if (!overview || length <= 0 || w <= 0)
        return;
    const trace::Pyramid &p = overview->pyramid;
    if (p.levels > 0 && !p.data.isEmpty()) {
        // All ranks combined per column from level 0: summed category shares, Compute = rest;
        // the largest wins (ties go to the MPI category).
        const int bins = p.binsPerLevel[0];
        const QVector<QVector<quint16>> &level = p.data[0];
        int runStart = 0;
        int runCat = -1;
        for (int x = 0; x <= w; ++x) {
            int cat = -1;
            if (x < w) {
                const int b0 = std::min(bins - 1, int(double(x) * bins / w));
                const int b1 = std::max(b0 + 1, std::min(bins, int(std::ceil(double(x + 1) * bins / w))));
                double share[kCategoryCount] = {};
                double total = 0;
                for (const QVector<quint16> &row : level) {
                    if (row.size() < bins * kCategoryCount)
                        continue;
                    for (int b = b0; b < b1; ++b) {
                        const quint16 *v = row.constData() + b * kCategoryCount;
                        for (int c = 1; c < kCategoryCount; ++c)
                            share[c] += v[c];
                        total += 65535;
                    }
                }
                double mpi = 0;
                for (int c = 1; c < kCategoryCount; ++c)
                    mpi += share[c];
                share[0] = std::max(0.0, total - mpi);
                cat = 0;
                for (int c = 1; c < kCategoryCount; ++c)
                    if (share[c] >= share[cat])
                        cat = c;
            }
            if (cat != runCat) {
                if (runCat >= 0)
                    painter->fillRect(QRect(runStart, 0, x - runStart, h), kCategoryColors[runCat]);
                runStart = x;
                runCat = cat;
            }
        }
    } else {
        painter->fillRect(0, 0, w, h, kCategoryColors[int(Category::Compute)]);
    }

    // Current window.
    if (m_viewEnd > m_viewStart) {
        const double x0 = double(std::clamp<qint64>(m_viewStart, 0, length)) / length * w;
        const double x1 = double(std::clamp<qint64>(m_viewEnd, 0, length)) / length * w;
        QPen pen(kWindowBorder, 2);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(x0 + 1, 1, std::max(1.0, x1 - x0 - 2), h - 2));
    }
}
