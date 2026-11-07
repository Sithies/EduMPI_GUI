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
