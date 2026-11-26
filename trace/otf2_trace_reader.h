#pragma once

#include <QDateTime>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

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
//
// progress (optional) is called on the reading thread while the events are read, about 200
// times per read: done = events read so far, total = meta.eventCount; the last call (after
// all events) has done == total. Not called if the read fails before the first event.
using ReadProgress = std::function<void(quint64 done, quint64 total)>;
bool readOtf2(const QString &anchorFile, TraceSink &sink, TraceMeta *meta, QString *error,
              const std::atomic_bool *cancel = nullptr, const ReadProgress &progress = {});

// A point of a read from which a later read of the same archive can resume: the position of
// every location's event reader, the reader's open regions and the sink's matching state
// (TraceSink::saveMatchState) just before the first event at or after its time.
struct ReadCheckpoint;

// Checkpoints of one archive, ascending in time (ReadOptions::buildIndex).
class TraceIndex
{
public:
    int size() const { return int(m_times.size()); }
    bool isEmpty() const { return m_times.isEmpty(); }
    qint64 timeNs(int i) const { return m_times[i]; }
    // The last checkpoint at or before ns; null if there is none.
    std::shared_ptr<const ReadCheckpoint> before(qint64 ns) const;
    // Its index (-1 if none) and the checkpoint at an index.
    int indexBefore(qint64 ns) const;
    std::shared_ptr<const ReadCheckpoint> at(int i) const { return m_points[i]; }
    void clear();
    void append(qint64 ns, std::shared_ptr<const ReadCheckpoint> checkpoint);

private:
    QVector<qint64> m_times;
    QVector<std::shared_ptr<const ReadCheckpoint>> m_points;
};

struct ReadOptions {
    const std::atomic_bool *cancel = nullptr;
    // done counts the events of this read (from resumeFrom, if given).
    ReadProgress progress;
    // Records about indexCheckpoints checkpoints, evenly spaced over the declared trace length,
    // into *buildIndex (cleared first). Use with a sink that sees every record (a whole-trace read).
    TraceIndex *buildIndex = nullptr;
    int indexCheckpoints = 256;
    // Starts reading at this checkpoint (of the same archive) instead of the beginning: the
    // sink gets addRank / begin() as usual, then restoreMatchState and the records from the
    // checkpoint on. Records of the same window equal those of a read from the start.
    std::shared_ptr<const ReadCheckpoint> resumeFrom;
    // Reads the OTF2 events on a worker thread while the calling thread runs the sink: same
    // calls in the same order, all on the calling thread (progress too). Only for reads of the
    // whole trace (no resumeFrom, no early stop: TraceSink::stopAfterNs() < 0 or unbounded);
    // ignored otherwise.
    bool pipelined = false;
};
bool readOtf2(const QString &anchorFile, TraceSink &sink, TraceMeta *meta, QString *error,
              const ReadOptions &options);

} // namespace trace
