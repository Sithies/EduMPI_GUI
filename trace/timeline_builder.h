#pragma once

#include <QHash>
#include <QVector>

#include <memory>

#include "timeline_store.h"
#include "trace_aggregator.h"
#include "trace_sink.h"

namespace trace {

// TraceSink in front of a TraceAggregator: forwards every record to it, registers itself as
// its listener and builds a TimelineStore from the records and the aggregator's matches.
//
// keepActivities: store activities, messages, collective entries and user spans overlapping
// [fromNs, toNs]. Without it only the user spans lasting at least one level-1 pyramid bin
// (declared length / 4096) are stored (for the overview phase band). The pyramid is filled whenever the range covers the whole run
// (fromNs <= 0 && toNs >= the trace length declared in begin()) and pyramidLevels > 0.
//
// Usage: readOtf2(anchor, builder, ...); agg.finish(len) (reports unmatched sends and
// incomplete collectives to the builder); builder.finish().
class TimelineBuilder : public TraceSink, private TraceListener
{
public:
    TimelineBuilder(TraceAggregator &agg, qint64 fromNs, qint64 toNs, bool keepActivities, int pyramidLevels);
    ~TimelineBuilder() override;

    void begin(const TraceMeta &meta) override;
    void addRank(int rank, const QString &node) override;
    void addCall(const MpiCall &call) override;
    void addSend(const P2PSend &send) override;
    void addRecv(const P2PRecv &recv) override;
    void addCollective(const Collective &coll) override;
    void addUserRegion(const UserRegion &region) override;

    // Call once, after agg.finish().
    std::shared_ptr<TimelineStore> finish();

private:
    void onMatched(const MatchedMessage &match) override;
    void onUnmatchedSend(const P2PSend &send) override;
    void onUnmatchedRecv(const P2PRecv &recv) override;
    void onCollective(const CompletedCollective &coll) override;

    // Link from the activity starting at startNs to a message / collective index; resolved in
    // finish() (activities may still be inserted out of order while reading).
    struct PendingLink {
        qint64 startNs;
        qint32 target;
    };
    // Recently patched receive-completing calls, used for the pyramid when activities are not
    // kept: the wait share already moved to Wait.
    struct RecentWait {
        qint64 startNs;
        qint64 endNs;
        qint64 waitNs;
    };
    struct RowState {
        QVector<PendingLink> links;
        QVector<RecentWait> recentWaits; // ring of kRecentWaits
        int recentNext = 0;
    };

    int rowIndex(int rank);
    quint16 functionIndex(const QString &name);
    bool overlapsWindow(qint64 start, qint64 end) const;
    int findActivity(int row, qint64 startNs, qint64 endNs) const;
    void addToPyramid(int row, qint64 start, qint64 end, Category cat, bool subtract);
    void patchWait(int row, const P2PRecv &recv, qint64 lateNs);
    void addMessage(const P2PSend &send, const P2PRecv *recv, double lateSender, double lateReceiver);

    TraceAggregator &m_agg;
    std::shared_ptr<TimelineStore> m_store;
    qint64 m_from;
    qint64 m_to;
    bool m_keep;
    int m_levels;
    bool m_pyramidOn = false;
    qint64 m_minOverviewSpanNs = 0; // declared length / 4096 (level-1 bin width)
    QVector<double> m_binsPerNs; // per level
    qint64 m_maxNs = 0;
    QVector<int> m_rankToRow; // dense lookup for ranks >= 0
    QVector<RowState> m_rowState;
    QHash<QString, quint16> m_functionIndex;
    QHash<quint64, QVector<int>> m_commMembers; // shared member list per communicator
};

} // namespace trace
