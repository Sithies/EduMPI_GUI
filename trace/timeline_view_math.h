#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include "timeline_store.h"

// Pure helpers of the timeline view: time axis, per-column sampling of rank rows and the phase
// band, hit testing and the messages drawn as arrows. All times are nanoseconds relative to trace
// start. Column i of a window [fromNs, toNs) split into n columns covers
// [fromNs + i * w, fromNs + (i + 1) * w) with w = (toNs - fromNs) / n (double).

namespace trace {

struct AxisTick {
    qint64 ns;
    double x;
    QString label;
};

// Ticks at multiples of the smallest step 1/2/5 * 10^n ns that is at least minSpacingPx wide,
// x = pixel position in [0, widthPx]. Unit by step: >= 1 s "s", >= 1 ms "ms", else "µs"; label
// integer if the step is a whole unit, else one decimal. Empty for an empty window or width.
QVector<AxisTick> axisTicks(qint64 fromNs, qint64 toNs, double widthPx, double minSpacingPx = 80);

struct ColumnSample {
    Category category;
    bool hasWait; // some wait time lies in the column
};

// Per column the category with the largest covered time (Compute = uncovered time, Wait its own
// category; ties go to the MPI category). Raw activities are used when a store holding them
// covers [fromNs, toNs] (detail, else overview if complete) and the column is narrower than the
// finest pyramid bin (or there is no pyramid); otherwise the pyramid level whose bins are the
// widest still <= the column width (the finest level if every bin is wider). O(columns +
// activities in view) resp. O(columns * bins per column). rowIndex refers to overview.rows; the
// detail row is looked up by rank. Empty for an invalid row / window / column count.
QVector<ColumnSample> sampleRow(const TimelineStore &overview, const TimelineStore *detail, int rowIndex,
                                qint64 fromNs, qint64 toNs, int columns);

struct HitResult {
    bool valid = false;
    int rank = -1;
    QString function;
    qint64 startNs = 0;
    qint64 durationNs = 0;
    qint64 waitNs = 0;
    int partner = -1;
    quint64 bytes = 0;
    quint32 tag = 0;
    quint64 comm = 0;
    int messageCount = 0;
    int messageIndex = -1; // into detail.messages: first P2P link of the call (also unmatched)
    QStringList callPath; // user functions open at the hit time, outermost first
    // Calls this one completes that carry another name, e.g. "MPI_Irecv from rank 3" for a
    // MPI_Wait; equal entries are counted ("2× MPI_Irecv from rank 0"). Empty for calls that
    // complete themselves (MPI_Recv, MPI_Send) and for collectives.
    QStringList completes;
};

// The activity containing ns (innermost if MPI calls nest), or the gap around ns as
// "Computation" (bounded by the neighbouring activities / the trace or loaded window). Partner,
// bytes, tag and comm come from the first link: for a P2P send the receiver (destination if
// unmatched), for a receive-completing call the sender; for a collective the bytes sent and the
// communicator. valid == false for an invalid row, ns outside the trace or outside the loaded
// window of an incomplete store.
HitResult activityAt(const TimelineStore &detail, int rowIndex, qint64 ns);

enum class PhaseLevel { Outermost, Innermost };

// Function id of the open user span at ns: Outermost = the depth-1 span if one is open (skips
// the program root main / MAIN__), else depth 0; Innermost = deepest open span; -1 if none.
int phaseFunctionAt(const TimelineStore &store, int rowIndex, qint64 ns, PhaseLevel level);

// Per column the function covering most of the column at that level (-1 = none wins).
QVector<int> samplePhaseBand(const TimelineStore &store, int rowIndex, qint64 fromNs, qint64 toNs, int columns,
                             PhaseLevel level);

// Indices into detail.messages of drawable messages (matched, sendNs >= 0) whose [sendNs, recvNs]
// overlaps [fromNs, toNs], in sendNs order, at most cap. rank -1 = all ranks, else messages that
// rank sent or received. Messages sent more than kMessageLookback messages before the window
// start are not considered (bounds the cost at deep zoom into long traces).
constexpr int kMessageLookback = 65536;
QVector<int> messagesInView(const TimelineStore &detail, int rank, qint64 fromNs, qint64 toNs, int cap);

// Per row of overview (same order) the category of the window [fromNs, toNs) as one column of
// sampleRow (raw activities when a store holding them covers the window, else the pyramid).
// Empty for an empty window.
QVector<ColumnSample> rankStatesInWindow(const TimelineStore &overview, const TimelineStore *detail, qint64 fromNs,
                                         qint64 toNs);

// Nearest event strictly after (forward) or before ns: the start or end of an activity (MPI
// call) of rank, or of any rank for rank -1. For an incomplete store only events within
// [store.fromNs, store.toNs] count, and ns before (forward) / after (backward) that window gives
// -1 (the events in between are not loaded). -1 if there is none (or the rank is unknown).
qint64 adjacentEventNs(const TimelineStore &store, int rank, qint64 ns, bool forward);

} // namespace trace
