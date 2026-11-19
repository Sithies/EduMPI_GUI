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

    // Not owned; may be null. Set before the first record.
    void setListener(TraceListener *listener) { m_listener = listener; }

private:
    struct Bucket {
        qint64 sendBytes = 0;
        qint64 recvBytes = 0;
        double lateSender = 0.0;
        double lateReceiver = 0.0;
        double timeDiff = 0.0;
    };
    // (rank, isCollective) -> bucket; QMap keeps output ordered by rank, p2p first.
    using BucketKey = QPair<int, bool>;

    struct DetailKey {
        bool collective;
        QString function;
        int rank;
        int partner;
        QByteArray collPartners;
        bool operator<(const DetailKey &o) const;
    };
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
    QVector<QMap<BucketKey, Bucket>> m_buckets;
    QVector<QMap<DetailKey, DetailSums>> m_details;
    QHash<MatchKey, MatchQueues> m_pending;
    QHash<QPair<int, quint64>, quint64> m_collSeq; // (rank, comm) -> next instance index
    QHash<QPair<quint64, quint64>, PendingCollective> m_pendingColl; // (comm, index)
};

} // namespace trace
