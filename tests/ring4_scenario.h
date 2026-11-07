#pragma once

#include "trace_aggregator.h"

// Reference scenario "ring4" (see the trace stage-1 plan): 4 ranks on nodeA/nodeB,
// a p2p ring in second 0, an MPI_Allreduce in second 1 and mixed p2p in second 2
// (incl. one never-received send). Shared by the aggregator and OTF2 reader tests.

// Feeds the scenario as neutral records (world ranks; WORLD = comm 1, SUB = comm 2).
void feedRing4(trace::TraceAggregator &agg);

// QCOMPARE/QVERIFY of the full expected bucket table, the matrix-relevant detail rows,
// unmatchedMessages == 1, seconds == 3 and nodes == {nodeA:[0,1], nodeB:[2,3]}.
void verifyRing4(const trace::TraceSummary &s);
