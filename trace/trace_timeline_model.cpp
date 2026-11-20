#include "trace_timeline_model.h"

#include <algorithm>

using trace::TimelineSnapshot;
using trace::TimelineStore;

TraceTimelineModel::TraceTimelineModel(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<trace::TimelineSnapshot>();
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &TraceTimelineModel::forwardRequest);
}

int TraceTimelineModel::rankCount() const
{
    return m_overview ? int(m_overview->rows.size()) : 0;
}

qint64 TraceTimelineModel::traceLengthNs() const
{
    return m_overview ? m_overview->traceLengthNs : 0;
}

bool TraceTimelineModel::windowed() const
{
    return m_overview && !m_overview->complete;
}

std::shared_ptr<const TimelineStore> TraceTimelineModel::detail() const
{
    if (m_overview && m_overview->complete)
        return m_overview;
    if (!m_detail)
        return {};
    if (m_detail->complete || !m_hasRequest)
        return m_detail;
    if (m_requestFrom >= m_detail->fromNs && m_requestTo <= m_detail->toNs)
        return m_detail;
    return {};
}

bool TraceTimelineModel::requestDetail(qint64 fromNs, qint64 toNs)
{
    if (!windowed())
        return false;
    const qint64 from = std::min(fromNs, toNs);
    const qint64 to = std::max(fromNs, toNs);
    if (m_maxDetailWindowNs > 0 && to - from > m_maxDetailWindowNs)
        return false; // cannot be loaded within the budget: the overview is shown instead
    m_hasRequest = true;
    m_requestFrom = from;
    m_requestTo = to;
    m_debounce.start(); // restarts a running timer
    emit changed();     // detail() may no longer match
    return true;
}

void TraceTimelineModel::forwardRequest()
{
    if (!m_hasRequest)
        return;
    // Already loaded for this range (and no other load pending that would replace it), or
    // being loaded right now: nothing to do.
    const bool covered = m_detail && m_requestFrom >= m_detail->fromNs && m_requestTo <= m_detail->toNs;
    if (covered && !m_loading)
        return;
    if (m_loading && m_forwardedFrom == m_requestFrom && m_forwardedTo == m_requestTo)
        return;
    // Asked before and answered without coverage (or failed): re-reading would not help,
    // except after an over-budget answer for a range at most half as long (zooming in); the
    // next over-budget answer becomes the new reference, so retries halve each time.
    if (!m_loading && m_hasUnanswered && m_requestFrom >= m_unansweredFrom && m_requestTo <= m_unansweredTo) {
        const qint64 span = m_requestTo - m_requestFrom;
        const qint64 refused = m_unansweredTo - m_unansweredFrom;
        if (!m_unansweredOverBudget || span >= refused || span * 2 > refused)
            return;
    }
    m_forwarded = true;
    m_forwardedFrom = m_requestFrom;
    m_forwardedTo = m_requestTo;
    m_loading = true;
    emit detailRequested(m_requestFrom, m_requestTo);
    emit changed();
}

void TraceTimelineModel::addSnapshot(const TimelineSnapshot &snapshot)
{
    if (!m_overview) {
        if (!snapshot.store)
            return;
        m_overview = snapshot.store;
        m_maxDetailWindowNs = std::max<qint64>(0, snapshot.maxDetailWindowNs);
        emit changed();
        return;
    }
    if (snapshot.store)
        m_detail = snapshot.store; // the previous window is released here
    if (m_forwarded && snapshot.requestFromNs == m_forwardedFrom && snapshot.requestToNs == m_forwardedTo)
        m_loading = false;
    const bool covered = snapshot.store
        && (snapshot.store->complete
            || (snapshot.requestFromNs >= snapshot.store->fromNs && snapshot.requestToNs <= snapshot.store->toNs));
    if (snapshot.requestFromNs >= 0) {
        m_hasUnanswered = !covered;
        m_unansweredOverBudget = !covered && snapshot.overBudget;
        m_unansweredFrom = snapshot.requestFromNs;
        m_unansweredTo = snapshot.requestToNs;
    }
    emit changed();
}
