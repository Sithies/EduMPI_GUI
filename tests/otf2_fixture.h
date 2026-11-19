#pragma once

#include <QString>

// OTF2 archives of the reference scenario "ring4" (see ring4_scenario.h), written with the
// OTF2 writer API: 4 process location groups with one CPU thread each, system tree
// machine -> nodeA (ranks 0, 1) / nodeB (ranks 2, 3), communicators WORLD and SUB = {2, 3}.
// Timer resolution 1e9 ticks/s, global offset 5 s, trace length 2.5 s.

// Writes <dir>/traces.otf2 (+ traces.def, traces/) and returns the anchor file path, or an
// empty string on failure. withRealtime = false omits the realtime timestamp.
QString writeRing4Otf2(const QString &dir, bool withRealtime = true);

// ring4 without R0's final Leave of main and without R0's MPI_Recv Leave.
QString writeTruncatedOtf2(const QString &dir);

// Same definitions as ring4 (plus MPI_Isend / MPI_Irecv / MPI_Wait / MPI_Waitall regions) with a
// nonblocking exchange between ranks 0 and 1 (see otf2_fixture.cpp, writeNonblockingRank):
// R0's Irecv (400 B from R1) completes in MPI_Wait [110, 250] ms, R1's Irecv (300 B from R0)
// in MPI_Waitall [300, 301] ms; R0 sends 300 B (Isend at 101 ms), R1 400 B (Isend at 200 ms).
QString writeNonblockingOtf2(const QString &dir);

// Like the nonblocking fixture, but R1's MPI_Waitall [150, 300] ms completes two Irecvs from R0
// (tag 2, 300 B, Isend at 200 ms; tag 3, 50 B, Isend at 250 ms), i.e. late-sender waits of 50 and
// 100 ms; R0 completes both Isends in MPI_Waitall [260, 261] ms. Ranks 2, 3 only run main.
QString writeWaitallOtf2(const QString &dir);

// 2 ranks (both on nodeA), WORLD = {0, 1}, trace length 3 s, user (COMPILER) regions:
// main [0, 3 s] > phase_a [0.1, 1.5 s] > send_to [0.5, 0.6 s] > MPI_Send [0.5, 0.501 s] to the
// other rank (tag 1, 100 B, received in MPI_Recv [0.7, 0.701 s] inside phase_a), and
// main > phase_b [1.5, 2.9 s] with MPI_Allreduce [1.6, 1.7 s] and the artificial region
// "TRACE BUFFER FLUSH" [2.0, 2.01 s] (role ARTIFICIAL, paradigm MEASUREMENT_SYSTEM).
QString writePhasesOtf2(const QString &dir);

// ring4 plus metric definitions: a "bytes_sent" metric member (type OTHER) and, with
// withClass, a PAPI member in a SYNCHRONOUS_STRICT metric class (as Score-P writes with PAPI).
QString writeMetricsOtf2(const QString &dir, bool withClass);

// Synthetic large trace: `ranks` ranks (first half on nodeA, rest on nodeB), WORLD = all ranks,
// main [0, length] on every rank, length = messagesPerRank * 100 us + 1 ms. Message i of rank r:
// MPI_Send [i * 100 us, +10 us] to (r + 1) % ranks (tag 0, 64 B) and MPI_Recv [i * 100 us + 20 us,
// +10 us] from (r - 1) % ranks. About 6 * messagesPerRank events per rank.
QString writeManyMessagesOtf2(const QString &dir, int ranks, int messagesPerRank);
