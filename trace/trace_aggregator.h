#pragma once

#include <QHash>
#include <QMap>
#include <QQueue>
#include <QString>
#include <QVector>

#include "trace_records.h"

namespace trace {

// Streaming aggregator: turns neutral trace records into per-second bucket and detail
// rows. Keeps only pending (unmatched) p2p records, incomplete collective instances and
// per-second aggregates; individual events are not stored.
class TraceAggregator
{
public:
    void addRank(int rank, const QString &node);
    void addCall(const MpiCall &call);
    void addSend(const P2PSend &send);
    void addRecv(const P2PRecv &recv);
    void addCollective(const Collective &coll);
    TraceSummary finish(qint64 traceLengthNs);

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

    struct PendingSend {
        qint64 enterNs;
        qint64 leaveNs;
    };
    struct PendingRecv {
        qint64 enterNs;
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
        QQueue<PendingSend> sends;
        QQueue<PendingRecv> recvs;
    };

    struct Arrival {
        int rank;
        qint64 enterNs;
    };
    struct PendingCollective {
        int expected = 0;
        QVector<Arrival> arrived;
    };

    static int secondOf(qint64 ns);
    Bucket &bucket(qint64 ns, int rank, bool collective);
    void addDetail(qint64 ns, const DetailKey &key, qint64 sendBytes, qint64 recvBytes);
    void bookMatch(int sender, int receiver, const PendingSend &s, const PendingRecv &r);
    void completeCollective(const PendingCollective &inst);

    QMap<int, QString> m_rankNodes;
    QVector<QMap<BucketKey, Bucket>> m_buckets;
    QVector<QMap<DetailKey, DetailSums>> m_details;
    QHash<MatchKey, MatchQueues> m_pending;
    QHash<QPair<int, quint64>, quint64> m_collSeq; // (rank, comm) -> next instance index
    QHash<QPair<quint64, quint64>, PendingCollective> m_pendingColl; // (comm, index)
};

} // namespace trace
