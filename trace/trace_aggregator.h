#pragma once

#include <QHash>
#include <QMap>
#include <QQueue>
#include <QString>
#include <QVector>

#include "trace_records.h"
#include "trace_sink.h"

namespace trace {

// Streaming aggregator: turns neutral trace records into per-second bucket and detail
// rows. Keeps only pending (unmatched) p2p records, incomplete collective instances and
// per-second aggregates; individual events are not stored. An optional listener sees every
// match and completed collective instance as it happens, and the unmatched p2p records /
// incomplete instances in finish(). User regions are ignored.
class TraceAggregator : public TraceSink
{
public:
    void addRank(int rank, const QString &node) override;
    void addCall(const MpiCall &call) override;
    void addSend(const P2PSend &send) override;
    void addRecv(const P2PRecv &recv) override;
    void addCollective(const Collective &coll) override;
    TraceSummary finish(qint64 traceLengthNs);
    // A pending (unmatched so far) p2p record or incomplete collective instance has a record
    // whose [enter, leave] overlaps [fromNs, toNs].
    bool waitingFor(qint64 fromNs, qint64 toNs) const;

    std::shared_ptr<const MatchState> saveMatchState() const override;
    void restoreMatchState(const std::shared_ptr<const MatchState> &state) override;

    // Not owned; may be null. Set before the first record.
    void setListener(TraceListener *listener) { m_listener = listener; }

private:
    friend struct MatchState;
    struct Bucket {
        qint64 sendBytes = 0;
        qint64 recvBytes = 0;
        double lateSender = 0.0;
        double lateReceiver = 0.0;
        double timeDiff = 0.0;
    };
    // (rank, isCollective) -> bucket; finish() orders the rows by rank, p2p first.
    using BucketKey = QPair<int, bool>;

    struct DetailKey {
        bool collective;
        QString function;
        int rank;
        int partner;
        QByteArray collPartners;
        quint64 comm; // the DB groups by communicator (communicationarea), too
        bool operator<(const DetailKey &o) const;
        bool operator==(const DetailKey &o) const;
    };
    friend size_t qHash(const DetailKey &k, size_t seed);
    struct DetailSums {
        qint64 sendBytes = 0;
        qint64 recvBytes = 0;
    };

    struct MatchKey {
        int sender;
        int receiver;
        quint64 comm;
        quint32 tag;
        bool operator==(const MatchKey &o) const
        {
            return sender == o.sender && receiver == o.receiver && comm == o.comm && tag == o.tag;
        }
    };
    friend size_t qHash(const MatchKey &k, size_t seed);
    struct MatchQueues {
        QQueue<P2PSend> sends;
        QQueue<P2PRecv> recvs;
    };

    // Bitmask of all members of a communicator (partnerMask without clearing a rank), built
    // once per member list instead of once per participation.
    struct CommMask {
        QVector<int> members;
        QByteArray mask;
    };
    QByteArray partnerMaskOf(const Collective &coll);

    struct PendingCollective {
        int expected = 0;
        QVector<Collective> arrived;
    };

    static int secondOf(qint64 ns);
    Bucket &bucket(qint64 ns, int rank, bool collective);
    void addDetail(qint64 ns, const DetailKey &key, qint64 sendBytes, qint64 recvBytes);
    void bookMatch(const P2PSend &s, const P2PRecv &r);
    void completeCollective(const PendingCollective &inst);

    TraceListener *m_listener = nullptr;
    QMap<int, QString> m_rankNodes;
    // Per second; hashed while reading, sorted once in finish() (same order as a QMap).
    QVector<QHash<BucketKey, Bucket>> m_buckets;
    QVector<QHash<DetailKey, DetailSums>> m_details;
    QHash<MatchKey, MatchQueues> m_pending;
    QHash<QPair<int, quint64>, quint64> m_collSeq; // (rank, comm) -> next instance index
    QHash<QPair<quint64, quint64>, PendingCollective> m_pendingColl; // (comm, index)
    QHash<quint64, CommMask> m_commMasks;
};

} // namespace trace
