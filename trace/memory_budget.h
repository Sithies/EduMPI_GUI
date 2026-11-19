#pragma once

#include <QtGlobal>

#include "trace_records.h"

// Memory budget of the trace timeline and the load decisions derived from it.

namespace trace {

// Physical memory currently available without swapping (macOS: free + inactive pages,
// Linux: MemAvailable); 2 GiB if it cannot be determined.
quint64 availableMemoryBytes();

// Budget for the timeline: env EDUMPI_TRACE_BUDGET_MB (> 0) overrides QSettings
// ("EduMPI", "EduMPI_GUI") key trace/memoryBudgetMB; 0 / missing = availableMemoryBytes() / 2.
quint64 memoryBudgetBytes();

// Pyramid levels tried first (finest dropped while the pyramid exceeds budget / 2).
constexpr int kDefaultPyramidLevels = 4;

struct TimelineLoadPlan {
    bool full = false; // keep all activities (complete store); else pyramid-only overview
    int pyramidLevels = 1;
};

// Levels: the most (<= kDefaultPyramidLevels, >= 1) with pyramidBytes(ranks, levels) <= budget / 2.
// Full mode if estimateTimelineBytes(...) * 1.5 <= budget (headroom for vector growth and the
// builder's pending links / member lists during the build).
TimelineLoadPlan planTimelineLoad(const TraceMeta &meta, int ranks, quint64 budget);

// Longest detail window [ns] whose estimated share of the timeline (events evenly spread over
// the run), with the same 1.5 headroom, stays within budget / 4: the overview (<= budget / 2),
// the detail still shown and the one being built then fit into the budget together.
// Returns the trace length if everything fits, at least 10 us.
qint64 maxDetailWindowNs(const TraceMeta &meta, int ranks, quint64 budget);

} // namespace trace
