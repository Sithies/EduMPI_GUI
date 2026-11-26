#pragma once

#include <QHash>
#include <QVector>

#include <atomic>
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

    // Byte cap for keepActivities builds (detail windows; 0 = none, the default). Each kept
    // record is counted with its links / indices (sizeof-based, comparable to bytesUsed()); a
    // record that would take the count past capBytes is dropped, overBudget() becomes true, no
    // further record is kept and *cancel (if given) is set so that readOtf2 stops early. The
    // store of an over-budget build is incomplete; callers discard it.
    void setByteCap(quint64 capBytes, std::atomic_bool *cancel = nullptr);
    bool overBudget() const { return m_overBudget; }
    quint64 keptBytes() const { return m_keptBytes; }

    // Downgrade cap for a complete build (keepActivities, whole range; 0 = none, the default),
    // counted like setByteCap. A record that would take the count past capBytes switches the
    // build to a pyramid-only overview instead of failing it: everything kept so far is
    // dropped (long user spans stay, as in an overview build) and the read continues. The
    // result equals an overview built with keepActivities = false. Unlike setByteCap the read
    // is not cancelled.
    void setDowngradeCap(quint64 capBytes) { m_downgradeCap = capBytes; }

    // Early stop (TraceSink::stopAfterNs) for window builds: the read ends once the window is
    // over and nothing in it waits for a partner. On by default; a whole-range build never
    // stops early.
    void setEarlyStop(bool on) { m_earlyStop = on; }
    qint64 stopAfterNs() const override { return m_earlyStop ? m_to : -1; }
    bool waitingForPartners() const override { return m_agg.waitingFor(m_from, m_to); }
    bool downgraded() const { return m_downgraded; }
    // keptBytes() when the downgrade happened (0 = not downgraded).
    quint64 keptBytesAtDowngrade() const { return m_keptAtDowngrade; }

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
    // categorize(), remembered per function name and kind (it searches the name).
    Category categoryOf(const QString &function, CallKind kind);
    bool overlapsWindow(qint64 start, qint64 end) const;
    int findActivity(int row, qint64 startNs, qint64 endNs) const;
    void addToPyramid(int row, qint64 start, qint64 end, Category cat, bool subtract);
    void patchWait(int row, const P2PRecv &recv, qint64 lateNs);
    void addMessage(const P2PSend *send, const P2PRecv *recv, double lateSender, double lateReceiver);
    // Accounts bytes for a record about to be kept; false (and over budget) past the cap.
    bool reserve(quint64 bytes);
    // Drops the kept records and continues as a pyramid-only overview (setDowngradeCap).
    void downgrade();

    TraceAggregator &m_agg;
    std::shared_ptr<TimelineStore> m_store;
    qint64 m_from;
    qint64 m_to;
    bool m_keep;
    int m_levels;
    bool m_pyramidOn = false;
    quint64 m_byteCap = 0;
    std::atomic_bool *m_capCancel = nullptr;
    quint64 m_keptBytes = 0;
    bool m_overBudget = false;
    quint64 m_downgradeCap = 0;
    bool m_earlyStop = true;
    bool m_downgraded = false;
    quint64 m_keptAtDowngrade = 0;
    qint64 m_minOverviewSpanNs = 0; // declared length / 4096 (level-1 bin width)
    QVector<double> m_binsPerNs; // per level
    qint64 m_maxNs = 0;
    QVector<int> m_rankToRow; // dense lookup for ranks >= 0
    QVector<RowState> m_rowState;
    QHash<QString, quint16> m_functionIndex;
    struct CategorySlot {
        quint8 known = 0; // bit per CallKind
        Category category[3] = {};
    };
    QHash<QString, CategorySlot> m_categories;
    QHash<quint64, QVector<int>> m_commMembers; // shared member list per communicator
};

} // namespace trace
