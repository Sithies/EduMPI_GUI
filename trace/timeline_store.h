#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include "trace_records.h"

// Per-rank timeline of a trace: MPI calls (activities), user-function spans, messages,
// collective participations and a fixed-size overview pyramid. Built by TimelineBuilder.
// All times are nanoseconds relative to trace start; ranks are world ranks.

namespace trace {

enum class Category : quint8 { Compute, Send, Recv, Collective, OtherMpi, Wait };
constexpr int kCategoryCount = 6;

// P2P with "send" in the name (case-insensitive) and without "recv" -> Send; other P2P
// (incl. MPI_Sendrecv, Wait*, Test*, Probe) -> Recv; Collective; OtherMpi.
Category categorize(const QString &function, CallKind kind);

// One MPI call. Links are RankRow::links[firstLink, firstLink + linkCount): indices into
// TimelineStore::messages (P2P: messages this call started or completed, several for
// MPI_Waitall / MPI_Testall) or into TimelineStore::collectives (collective call, one link).
// waitNs (calls that complete receives) = max late-sender wait of those messages, capped at
// the call's duration; drawn at the start of the bar.
struct Activity {
    qint64 startNs;
    qint64 endNs;
    qint64 waitNs;
    qint32 firstLink;
    quint16 linkCount;
    quint16 function; // index into TimelineStore::functions
    Category category;
};

// One user-function region; depth 0 = program root, capped at kMaxUserDepth (deeper regions
// are stored with depth kMaxUserDepth). Which spans a store holds:
// - built with keepActivities (full store or detail window): every span overlapping
//   [fromNs, toNs];
// - built without (pyramid-only overview): only spans lasting at least one level-1 pyramid
//   bin (declared trace length / 4096); shorter ones are invisible at overview zoom. This
//   bounds the count to about 4096 per rank and depth. With an unknown (0) declared length
//   all spans are kept.
struct UserSpan {
    qint64 startNs;
    qint64 endNs;
    quint16 function;
    quint8 depth;
};
constexpr int kMaxUserDepth = 15;

// sendNs = enter of the send call, recvNs = leave of the call completing the receive.
// Unmatched sends: receiver = -1, recvNs = -1 (unmatched receives are not stored).
struct Message {
    int sender;
    int receiver;
    qint64 sendNs;
    qint64 recvNs;
    quint64 bytes;
    quint32 tag;
    quint64 comm;
    double lateSender;   // seconds
    double lateReceiver; // seconds
};

// One rank's participation in a collective instance.
struct CollectiveEntry {
    int rank;
    quint64 comm;
    QVector<int> members;
    quint64 bytesSent;
    quint64 bytesRecv;
    double lateSender; // seconds
};

struct RankRow {
    int rank = -1;
    QString node;
    QVector<Activity> activities; // sorted by startNs
    QVector<qint32> links;
    QVector<qint32> messages;     // indices of messages sent or received, sorted by sendNs
    QVector<UserSpan> userSpans;  // sorted by (startNs, depth)
    quint8 maxDepth = 0;
};

// Overview of the whole run: level k has 1024 * 4^k bins; data[level][rowIndex] holds
// bins * kCategoryCount values (bin-major), each the fraction of the bin's time spent in that
// category * 65535 (Compute = remainder). Empty (levels == 0) unless built over the full range.
struct Pyramid {
    int levels = 0;
    QVector<int> binsPerLevel;
    qint64 traceLengthNs = 0;
    QVector<QVector<QVector<quint16>>> data;
};

class TimelineStore
{
public:
    QVector<RankRow> rows; // ascending rank
    QStringList functions; // shared by MPI and user functions; at most 65536 entries, the
                           // last being "(other)" once more distinct names occur
    QVector<Message> messages; // sorted by sendNs
    QVector<CollectiveEntry> collectives;
    Pyramid pyramid;
    qint64 traceLengthNs = 0;
    qint64 fromNs = 0;
    qint64 toNs = 0;
    bool complete = false; // activities cover the whole run; otherwise only [fromNs, toNs]
    int unmatchedMessages = 0;

    quint64 bytesUsed() const; // payload bytes (O(ranks) bookkeeping excluded)
    int rowIndexOfRank(int rank) const; // -1 if unknown
};

// Bytes of a pyramid with the given number of rank rows and levels.
quint64 pyramidBytes(int ranks, int levels);

// Upper-bound estimate of a full timeline (activities, messages, user spans, pyramid) for a
// trace with eventCount events (METRIC events, ~40 % with hasMetrics, carry no timeline data).
quint64 estimateTimelineBytes(quint64 eventCount, bool hasMetrics, int ranks, int pyramidLevels);

} // namespace trace
