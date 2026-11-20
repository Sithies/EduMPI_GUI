#pragma once

#include <QObject>
#include <QTimer>

#include <memory>

#include "timeline_snapshot.h"

// GUI-thread view of the timeline snapshots a Trace_Data_Source delivers. The first snapshot
// is the overview (complete store in full mode, pyramid-only in windowed mode); later ones are
// detail windows answering requestDetail.
class TraceTimelineModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int rankCount READ rankCount NOTIFY changed)
    Q_PROPERTY(qint64 traceLengthNs READ traceLengthNs NOTIFY changed)
    Q_PROPERTY(bool windowed READ windowed NOTIFY changed)
    Q_PROPERTY(bool loadingDetail READ loadingDetail NOTIFY changed)
    // Longest range requestDetail accepts (0 = no limit); wider views show the overview.
    Q_PROPERTY(qint64 maxDetailWindowNs READ maxDetailWindowNs NOTIFY changed)

public:
    static constexpr int kDebounceMs = 250;

    explicit TraceTimelineModel(QObject *parent = nullptr);

    int rankCount() const;
    qint64 traceLengthNs() const;
    bool windowed() const; // overview is not complete: details are loaded on demand
    bool loadingDetail() const { return m_loading; }
    qint64 maxDetailWindowNs() const { return m_maxDetailWindowNs; }

    std::shared_ptr<const trace::TimelineStore> overview() const { return m_overview; }
    // The complete overview in full mode; in windowed mode the latest detail window if it
    // covers the last requested range (any detail window before the first request); else null.
    std::shared_ptr<const trace::TimelineStore> detail() const;

    // Debounced (kDebounceMs): only the last range of a burst is forwarded as detailRequested.
    // Returns false (and does nothing) in full mode or for a range longer than
    // maxDetailWindowNs. A range equal to or inside the last answered request that came back
    // failed or without covering it is not forwarded again, unless that answer was over the
    // memory budget and the new range is at most half as long.
    Q_INVOKABLE bool requestDetail(qint64 fromNs, qint64 toNs);

public slots:
    void addSnapshot(const trace::TimelineSnapshot &snapshot);

signals:
    void changed();
    void detailRequested(qint64 fromNs, qint64 toNs);

private:
    void forwardRequest();

    std::shared_ptr<const trace::TimelineStore> m_overview;
    std::shared_ptr<const trace::TimelineStore> m_detail;
    bool m_hasRequest = false;
    qint64 m_requestFrom = 0; // last range passed to requestDetail
    qint64 m_requestTo = 0;
    bool m_forwarded = false;
    qint64 m_forwardedFrom = 0; // last range forwarded to the source
    qint64 m_forwardedTo = 0;
    bool m_loading = false;
    qint64 m_maxDetailWindowNs = 0;
    // Last answered request that failed (null store) or was not covered by its answer.
    bool m_hasUnanswered = false;
    bool m_unansweredOverBudget = false; // it did not fit the memory budget (see forwardRequest)
    qint64 m_unansweredFrom = 0;
    qint64 m_unansweredTo = 0;
    QTimer m_debounce;
};
