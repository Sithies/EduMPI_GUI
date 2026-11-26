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
struct MemoryBudget {
    enum Source { Environment, Setting, Automatic };
    quint64 bytes = 1;
    Source source = Automatic;
    quint64 availableBytes = 0; // availableMemoryBytes() when resolved (also for the others)
};
MemoryBudget resolveMemoryBudget();
quint64 memoryBudgetBytes(); // resolveMemoryBudget().bytes

// The user setting behind memoryBudgetBytes(): QSettings ("EduMPI", "EduMPI_GUI") key
// trace/memoryBudgetMB in MB, 0 = automatic. Negative values are stored as 0.
int memoryBudgetSettingMB();
void setMemoryBudgetSettingMB(int mb);

// Pyramid levels tried first (finest dropped while the pyramid exceeds budget / 2).
constexpr int kDefaultPyramidLevels = 4;

struct TimelineLoadPlan {
    int pyramidLevels = 1;
    // TimelineBuilder::setDowngradeCap for the load: the load starts as a complete build and
    // switches to a pyramid-only overview once the kept records reach this many bytes.
    quint64 downgradeCapBytes = 1;
};

// Levels: the most (<= kDefaultPyramidLevels, >= 1) with pyramidBytes(ranks, levels) <= budget / 2.
// Downgrade cap: (budget - pyramid) / 1.5 (headroom for vector growth and the builder's
// pending links / member lists), at least 1. The decision between complete store and overview
// is thus made from the bytes really kept while reading, not from an estimate.
TimelineLoadPlan planTimelineLoad(int ranks, quint64 budget);

// Longest detail window [ns] whose share of the timeline (events evenly spread over the run),
// with the same 1.5 headroom, stays within budget / 4: the overview (<= budget / 2), the detail
// still shown and the one being built then fit into the budget together. bytesPerEvent: kept
// bytes per OTF2 event measured during the load (0 = unknown: estimateTimelineBytes is used).
// Returns the trace length if everything fits, at least 10 us. A window denser than average is
// stopped at runtime by the builder's byte cap (budget / 4, Trace_Data_Source::processDetailRequest).
qint64 maxDetailWindowNs(const TraceMeta &meta, int ranks, quint64 budget, double bytesPerEvent = 0);

} // namespace trace
