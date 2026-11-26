#pragma once

#include <QHash>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class TraceTimelineModel;

// Per-rank timeline of a trace (TraceTimelineModel): time axis on top, ranks grouped by node
// (node header), per rank an optional phase band (user function) above the MPI-call bar, and
// message arrows. Compiled only with EDUMPI_TRACE; QML sees the model as a plain QObject*.
//
// Detail loading: only when the view range changes (zoom, pan, resize, view properties, model)
// and only in windowed mode, when the model's detail does not cover the view and the view is at
// most maxDetailWindowNs long, model.requestDetail(view) is called; until then (and for wider
// views) the overview pyramid is drawn. model.changed() only repaints. A hidden item (e.g. a
// page a StackView keeps below the visible one) requests nothing; it checks once when it
// becomes visible again (the model answers only its latest request).
class TraceTimelineItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(qint64 viewStartNs READ viewStartNs WRITE setViewStartNs NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewEndNs READ viewEndNs WRITE setViewEndNs NOTIFY viewChanged)
    Q_PROPERTY(int selectedRank READ selectedRank WRITE setSelectedRank NOTIFY selectedRankChanged)
    // A link from the Trace Graph (sender, receiver; -1 = none): its messages in the window are
    // drawn as orange arrows, both rows lit, the rest dimmed (takes precedence over selectedRank).
    Q_PROPERTY(int linkFrom READ linkFrom WRITE setLinkFrom NOTIFY displayChanged)
    Q_PROPERTY(int linkTo READ linkTo WRITE setLinkTo NOTIFY displayChanged)
    Q_PROPERTY(bool allArrows READ allArrows WRITE setAllArrows NOTIFY displayChanged)
    Q_PROPERTY(bool highlightWait READ highlightWait WRITE setHighlightWait NOTIFY displayChanged)
    Q_PROPERTY(bool showArrows READ showArrows WRITE setShowArrows NOTIFY displayChanged)
    Q_PROPERTY(bool showPhases READ showPhases WRITE setShowPhases NOTIFY displayChanged)
    Q_PROPERTY(QString phaseLevel READ phaseLevel WRITE setPhaseLevel NOTIFY displayChanged)
    Q_PROPERTY(qreal contentY READ contentY WRITE setContentY NOTIFY contentYChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY contentHeightChanged)
    Q_PROPERTY(bool arrowsCapped READ arrowsCapped NOTIFY arrowsCappedChanged)

public:
    static constexpr int kLabelWidth = 48;
    static constexpr int kNodeHeader = 16;
    static constexpr int kPhaseBand = 12;
    static constexpr int kBarRow = 18;
    static constexpr int kAxisHeight = 22;
    static constexpr int kArrowCap = 2000;
    static constexpr qint64 kMinWindowNs = 10'000; // 10 µs

    explicit TraceTimelineItem(QQuickItem *parent = nullptr);

    QObject *model() const;
    void setModel(QObject *model);
    qint64 viewStartNs() const { return m_viewStart; }
    void setViewStartNs(qint64 ns);
    qint64 viewEndNs() const { return m_viewEnd; }
    void setViewEndNs(qint64 ns);
    int selectedRank() const { return m_selectedRank; }
    int linkFrom() const { return m_linkFrom; }
    void setLinkFrom(int rank);
    int linkTo() const { return m_linkTo; }
    void setLinkTo(int rank);
    // Messages drawn as arrows (indices into the detail store's messages), for tests.
    const QVector<int> &arrowMessages() const { return m_arrows; }
    void setSelectedRank(int rank);
    bool allArrows() const { return m_allArrows; }
    void setAllArrows(bool on);
    bool highlightWait() const { return m_highlightWait; }
    void setHighlightWait(bool on);
    bool showArrows() const { return m_showArrows; }
    void setShowArrows(bool on);
    bool showPhases() const { return m_showPhases; }
    void setShowPhases(bool on);
    QString phaseLevel() const { return m_innerPhases ? QStringLiteral("inner") : QStringLiteral("outer"); }
    void setPhaseLevel(const QString &level);
    qreal contentY() const { return m_contentY; }
    void setContentY(qreal y);
    qreal contentHeight() const { return m_contentHeight; }
    bool arrowsCapped() const { return m_arrowsCapped; }

    // The time under x stays fixed; the window is clamped to [10 µs, trace length] and into
    // [0, trace length].
    Q_INVOKABLE void zoomAt(qreal x, qreal factor);
    // Moves the window by dx pixels (positive = later), clamped to the trace.
    Q_INVOKABLE void panBy(qreal dx);
    Q_INVOKABLE void showFullRun();
    // Sets both ends at once (no inverted intermediate range), clamped like zoomAt / panBy:
    // span to [10 µs, trace length], window into [0, trace length].
    Q_INVOKABLE void setView(qint64 start, qint64 end);
    // Label area: toggles selectedRank; phase band / bar: emits barClicked with the activity
    // (or computation gap) under the point; elsewhere nothing.
    Q_INVOKABLE void clickAt(qreal x, qreal y);

    void paint(QPainter *painter) override;

signals:
    void modelChanged();
    void viewChanged();
    void selectedRankChanged();
    void displayChanged();
    void contentYChanged();
    void contentHeightChanged();
    void arrowsCappedChanged();
    // Keys: function, rank, startUs, durationUs, waitUs, partner, bytes, tag, comm, callPath
    // (QStringList, outermost first), messageCount.
    void barClicked(QVariantMap details);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    struct RowLayout {
        int rowIndex; // into overview rows
        int rank;
        qreal top;    // content y of the phase band (= bar top without phases)
        qreal barTop; // content y of the bar
    };
    struct HeaderLayout {
        QString node;
        qreal top;
    };

    void onModelChanged();
    void rebuildLayout();
    void viewRangeChanged();   // GUI thread: clamp, request detail if needed, derived state
    void maybeRequestDetail();
    void updateDerived();      // arrows (capped) and partner ranks of the selected rank
    qint64 traceLength() const;
    double plotWidth() const;
    const RowLayout *rowAtY(qreal y) const;

    QPointer<TraceTimelineModel> m_model;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    int m_selectedRank = -1;
    int m_linkFrom = -1;
    int m_linkTo = -1;
    bool m_allArrows = false;
    bool m_highlightWait = true;
    bool m_showArrows = true;
    bool m_showPhases = true;
    bool m_innerPhases = false;
    qreal m_contentY = 0;
    qreal m_contentHeight = 0;
    bool m_arrowsCapped = false;

    QVector<RowLayout> m_rows;
    QVector<HeaderLayout> m_headers;
    QHash<int, int> m_layoutOfRank; // rank -> index into m_rows

    // Derived on the GUI thread (paint only reads them).
    QVector<int> m_arrows;   // indices into detail messages
    QHash<int, bool> m_partners; // ranks exchanging a message with the selected rank in view
    bool m_dimOthers = false;

    bool m_requested = false;
    qint64 m_requestedFrom = 0;
    qint64 m_requestedTo = 0;
};
