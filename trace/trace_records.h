#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <QtGlobal>

// Neutral (OTF2-independent) trace records fed into TraceAggregator, and the
// per-second rows it produces. All times are nanoseconds relative to trace start;
// all ranks are world ranks.

namespace trace {

enum class CallKind { P2P, Collective, OtherMpi };

// Classifies an MPI function name (e.g. "MPI_Isend") by communication type.
CallKind classifyMpiFunction(const QString &name);

// One MPI region of a rank; used for time_diff (sum of call durations).
struct MpiCall {
    int rank;
    CallKind kind;
    QString function;
    qint64 enterNs;
    qint64 leaveNs;
};

// rank = sender, peer = receiver; times = enclosing MPI region.
struct P2PSend {
    int rank;
    int peer;
    quint64 comm;
    quint32 tag;
    quint64 bytes;
    QString function;
    qint64 enterNs;
    qint64 leaveNs;
};

// rank = receiver, peer = sender; times = enclosing MPI region.
struct P2PRecv {
    int rank;
    int peer;
    quint64 comm;
    quint32 tag;
    quint64 bytes;
    QString function;
    qint64 enterNs;
    qint64 leaveNs;
};

// One rank's participation in a collective; members = world ranks of the communicator.
struct Collective {
    int rank;
    quint64 comm;
    QVector<int> members;
    QString function;
    qint64 enterNs;
    qint64 leaveNs;
    quint64 bytesSent;
    quint64 bytesRecv;
};

// Same shape as Database_Thread's DataColumn: one row per (second, rank, commType).
// lateSender / lateReceiver / timeDiff are in seconds.
struct BucketRow {
    QString node;
    int rank;
    QString commType; // "p2p" or "collective"
    qint64 sendBytes;
    qint64 recvBytes;
    double lateSender;
    double lateReceiver;
    double timeDiff;
};

// Per-second detail row. p2p: byte sums per (function, rank, partner).
// collective: partner = -1, collPartners = bitmask of the other members' world ranks
// (bit i of byte i/8, as decoded by Detailed_coll_data::queryData); bytes are the
// rank's undistributed totals (the matrix builder divides them over the partners).
struct DetailRow {
    QString commType;
    QString function;
    int rank;
    int partner;
    QByteArray collPartners;
    qint64 sendBytes;
    qint64 recvBytes;
};

struct TraceSummary {
    QMap<QString, QVector<int>> nodes;
    int rankCount = 0;
    int seconds = 0;
    int unmatchedMessages = 0;
    QVector<QVector<BucketRow>> rows;    // indexed by second
    QVector<QVector<DetailRow>> details; // indexed by second
};

} // namespace trace
