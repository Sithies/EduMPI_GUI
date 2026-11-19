#pragma once

#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

class TraceTimelineModel;

// Overview strip of the whole run: per column the dominant category of all ranks combined
// (pyramid level 0) and the current view window as a bordered rectangle. Clicking or dragging
// moves the window (size kept, clamped to the trace) and emits windowMoved. Compiled only with
// EDUMPI_TRACE; QML sees the model as a plain QObject*.
class TraceOverviewItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(qint64 viewStartNs READ viewStartNs WRITE setViewStartNs NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewEndNs READ viewEndNs WRITE setViewEndNs NOTIFY viewChanged)

public:
    explicit TraceOverviewItem(QQuickItem *parent = nullptr);

    QObject *model() const;
    void setModel(QObject *model);
    qint64 viewStartNs() const { return m_viewStart; }
    void setViewStartNs(qint64 ns);
    qint64 viewEndNs() const { return m_viewEnd; }
    void setViewEndNs(qint64 ns);

    // Centres the window (size kept) at item x, clamped to [0, trace length]; emits windowMoved.
    Q_INVOKABLE void moveWindowTo(qreal x);

    void paint(QPainter *painter) override;

signals:
    void modelChanged();
    void viewChanged();
    void windowMoved(qint64 startNs, qint64 endNs);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    qint64 traceLength() const;

    QPointer<TraceTimelineModel> m_model;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
};
