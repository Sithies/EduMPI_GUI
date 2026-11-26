#pragma once

#include <memory>

#include "trace_records.h"

// Interfaces between trace stages: the OTF2 reader feeds a TraceSink (TraceAggregator, or a
// TimelineBuilder that forwards to one); TraceAggregator reports its p2p matches and
// completed collective instances to an optional TraceListener.

namespace trace {

// Matching state of a TraceAggregator (pending p2p records, incomplete collective instances,
// collective instance counters); defined in trace_aggregator.cpp. Saved at the checkpoints of
// a trace index and restored when a read resumes from one (otf2_trace_reader.h).
struct MatchState;

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

    // Early stop of a read (detail windows): records of events after stopAfterNs() are not
    // needed (-1 = read everything). Once past that time the reader stops as soon as no MPI
    // call entered up to then is still open and waitingForPartners() is false (asked about
    // every few thousand events); user regions still open are closed at the trace end.
    virtual qint64 stopAfterNs() const { return -1; }
    // Records overlapping the needed range still wait for their partner (p2p match or the
    // rest of a collective instance), which may come after stopAfterNs().
    virtual bool waitingForPartners() const { return false; }

    // Checkpoints (TraceIndex): the matching state at this point of the read, and restoring it
    // before a read that resumes from a checkpoint (after begin()). Sinks without matching
    // state ignore both; forwarding sinks pass them on to their aggregator.
    virtual std::shared_ptr<const MatchState> saveMatchState() const { return {}; }
    virtual void restoreMatchState(const std::shared_ptr<const MatchState> &state) { Q_UNUSED(state); }
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
