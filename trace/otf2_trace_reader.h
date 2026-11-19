#pragma once

#include <QDateTime>
#include <QString>

#include <atomic>

#include "trace_aggregator.h"
#include "trace_sink.h"

// Translates an OTF2 archive (Score-P, later EduMPI) into the neutral records consumed by
// a TraceSink (usually TraceAggregator, or a TimelineBuilder in front of it): ranks, MPI
// regions, MPI communication events and user (non-MPI, non-artificial) regions.

namespace trace {

// Accepts file:// URLs and plain paths; a directory resolves to <dir>/traces.otf2.
// Returns an empty string and sets *error if nothing suitable exists.
QString resolveTracePath(const QString &urlOrPath, QString *error);

// Reads the archive whose anchor file is anchorFile and feeds it into sink: addRank for
// every rank, then sink.begin(meta) (name, realtimeStart, eventCount, hasMetrics and the
// declared traceLengthNs filled), then all records in globally time-merged order. *meta
// (may be null) holds the final values afterwards (traceLengthNs extended to the last
// event). Returns false with a readable *error for unreadable / non-OTF2 input, and with
// "Cancelled" once *cancel becomes true.
bool readOtf2(const QString &anchorFile, TraceSink &sink, TraceMeta *meta, QString *error,
              const std::atomic_bool *cancel = nullptr);

} // namespace trace
