#pragma once

#include <QDateTime>
#include <QString>

#include <atomic>

#include "trace_aggregator.h"

// Translates an OTF2 archive (Score-P, later EduMPI) into the neutral records consumed by
// TraceAggregator. Only MPI regions and MPI communication events are used in stage 1.

namespace trace {

struct TraceMeta {
    QString name;            // name of the directory containing the anchor file
    QDateTime realtimeStart; // UTC wall clock of trace start; invalid if absent
    qint64 traceLengthNs = 0;
};

// Accepts file:// URLs and plain paths; a directory resolves to <dir>/traces.otf2.
// Returns an empty string and sets *error if nothing suitable exists.
QString resolveTracePath(const QString &urlOrPath, QString *error);

// Reads the archive whose anchor file is anchorFile and feeds every rank and MPI record
// into agg (events in globally time-merged order). Returns false with a readable *error
// for unreadable / non-OTF2 input, and with "Cancelled" once *cancel becomes true.
bool readOtf2(const QString &anchorFile, TraceAggregator &agg, TraceMeta *meta, QString *error,
              const std::atomic_bool *cancel = nullptr);

} // namespace trace
