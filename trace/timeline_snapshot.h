#pragma once

#include <QMetaType>
#include <QtGlobal>

#include <memory>

#include "timeline_store.h"

namespace trace {

// Immutable timeline handed from the loading thread to the GUI thread (queued signals).
// requestFromNs / requestToNs: the detail request this snapshot answers (-1 for the
// overview loaded with the trace). store may be null when a detail load failed or the source
// was stopped. maxDetailWindowNs (overview only): longest range a detail request can cover
// within the memory budget (0 = unknown / no limit).
struct TimelineSnapshot {
    std::shared_ptr<const TimelineStore> store;
    qint64 requestFromNs = -1;
    qint64 requestToNs = -1;
    qint64 maxDetailWindowNs = 0;
};

} // namespace trace

Q_DECLARE_METATYPE(trace::TimelineSnapshot)
