#pragma once

#include "trace_records.h"

// Interfaces between trace stages: the OTF2 reader feeds a TraceSink (TraceAggregator, or a
// TimelineBuilder that forwards to one); TraceAggregator reports its p2p matches and
// completed collective instances to an optional TraceListener.

namespace trace {

class TraceSink
{
public:
    virtual ~TraceSink() = default;
    // Called once after all addRank calls and before the first record.
    virtual void begin(const TraceMeta &meta) { Q_UNUSED(meta); }
    virtual void addRank(int rank, const QString &node) = 0;
    virtual void addCall(const MpiCall &call) = 0;
    virtual void addSend(const P2PSend &send) = 0;
    virtual void addRecv(const P2PRecv &recv) = 0;
    virtual void addCollective(const Collective &coll) = 0;
    virtual void addUserRegion(const UserRegion &region) { Q_UNUSED(region); }
};

class TraceListener
{
public:
    virtual ~TraceListener() = default;
    virtual void onMatched(const MatchedMessage &match) { Q_UNUSED(match); }
    virtual void onUnmatchedSend(const P2PSend &send) { Q_UNUSED(send); }
    virtual void onUnmatchedRecv(const P2PRecv &recv) { Q_UNUSED(recv); }
    virtual void onCollective(const CompletedCollective &coll) { Q_UNUSED(coll); }
};

} // namespace trace
