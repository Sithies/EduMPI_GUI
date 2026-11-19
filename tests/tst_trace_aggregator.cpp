#include <QtTest>

#include "ring4_scenario.h"
#include "trace_aggregator.h"

#include <cmath>

using namespace trace;

namespace {
constexpr qint64 kMs = 1'000'000;

struct RecordingListener : TraceListener {
    QVector<MatchedMessage> matched;
    QVector<P2PSend> unmatchedSends;
    QVector<P2PRecv> unmatchedRecvs;
    QVector<CompletedCollective> collectives;
    void onMatched(const MatchedMessage &m) override { matched.append(m); }
    void onUnmatchedSend(const P2PSend &s) override { unmatchedSends.append(s); }
    void onUnmatchedRecv(const P2PRecv &r) override { unmatchedRecvs.append(r); }
    void onCollective(const CompletedCollective &c) override { collectives.append(c); }
};

const MatchedMessage *findMatch(const QVector<MatchedMessage> &all, int from, int to, quint32 tag)
{
    for (const MatchedMessage &m : all)
        if (m.send.rank == from && m.send.peer == to && m.recv.rank == to && m.recv.peer == from
            && m.send.tag == tag && m.recv.tag == tag)
            return &m;
    return nullptr;
}

bool fuzzy(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}
} // namespace

class TstTraceAggregator : public QObject
{
    Q_OBJECT
private slots:
    void ring4Aggregates()
    {
        TraceAggregator a;
        feedRing4(a);
        verifyRing4(a.finish(2'500'000'000));
    }

    void listenerSeesRing4Matches()
    {
        TraceAggregator a;
        RecordingListener l;
        a.setListener(&l);
        feedRing4(a);
        const TraceSummary s = a.finish(2'500'000'000);
        verifyRing4(s); // numbers unchanged with a listener

        QCOMPARE(l.matched.size(), 6);
        const MatchedMessage *r3r0 = findMatch(l.matched, 3, 0, 7);
        QVERIFY(r3r0);
        QVERIFY2(fuzzy(r3r0->lateSender, 0.299), qPrintable(QString::number(r3r0->lateSender)));
        QCOMPARE(r3r0->lateReceiver, 0.0);
        QCOMPARE(r3r0->send.enterNs, 400 * kMs);
        QCOMPARE(r3r0->send.leaveNs, 401 * kMs);
        QCOMPARE(r3r0->recv.enterNs, 101 * kMs);
        QCOMPARE(r3r0->recv.leaveNs, 402 * kMs);
        QCOMPARE(r3r0->send.bytes, quint64(1000));
        QCOMPARE(r3r0->recv.function, QStringLiteral("MPI_Recv"));

        const MatchedMessage *r1r2 = findMatch(l.matched, 1, 2, 9);
        QVERIFY(r1r2);
        QVERIFY2(fuzzy(r1r2->lateReceiver, 0.300), qPrintable(QString::number(r1r2->lateReceiver)));
        QCOMPARE(r1r2->lateSender, 0.0);
        QCOMPARE(r1r2->send.function, QStringLiteral("MPI_Ssend"));

        QCOMPARE(l.unmatchedSends.size(), 1);
        QCOMPARE(l.unmatchedSends[0].rank, 3);
        QCOMPARE(l.unmatchedSends[0].peer, 0);
        QCOMPARE(l.unmatchedSends[0].tag, quint32(11));
        QCOMPARE(l.unmatchedSends[0].bytes, quint64(100));
        QCOMPARE(l.unmatchedRecvs.size(), 0);

        QCOMPARE(l.collectives.size(), 1);
        const CompletedCollective &c = l.collectives[0];
        QCOMPARE(c.members.size(), 4);
        QCOMPARE(c.lateSender.size(), 4);
        QVector<double> byRank(4, -1.0);
        for (int i = 0; i < c.members.size(); ++i) {
            QCOMPARE(c.members[i].function, QStringLiteral("MPI_Allreduce"));
            byRank[c.members[i].rank] = c.lateSender[i];
        }
        const double expected[] = {0.2, 0.2, 0.0, 0.2};
        for (int r = 0; r < 4; ++r)
            QVERIFY2(fuzzy(byRank[r], expected[r]), qPrintable(QStringLiteral("rank %1: %2").arg(r).arg(byRank[r])));
    }

    // Instances still incomplete at finish() are reported with the members that arrived;
    // unmatched receives are reported too.
    void listenerSeesIncompleteAtFinish()
    {
        TraceAggregator a;
        RecordingListener l;
        a.setListener(&l);
        a.addRecv({2, 3, 1, 4, 300, QStringLiteral("MPI_Recv"), 10 * kMs, 20 * kMs});
        a.addCollective({0, 1, {0, 1}, QStringLiteral("MPI_Barrier"), 30 * kMs, 40 * kMs, 0, 0});
        QCOMPARE(l.collectives.size(), 0);
        a.finish(1'000'000'000);
        QCOMPARE(l.unmatchedRecvs.size(), 1);
        QCOMPARE(l.unmatchedRecvs[0].bytes, quint64(300));
        QCOMPARE(l.collectives.size(), 1);
        QCOMPARE(l.collectives[0].members.size(), 1);
        QCOMPARE(l.collectives[0].lateSender, QVector<double>{0.0});
    }

    void matchesRecvBeforeSend()
    {
        TraceAggregator a;
        a.addRank(0, QStringLiteral("n"));
        a.addRank(1, QStringLiteral("n"));
        a.addRecv({1, 0, 1, 7, 1000, QStringLiteral("MPI_Recv"), 101'000'000, 102'000'000});
        a.addSend({0, 1, 1, 7, 1000, QStringLiteral("MPI_Send"), 100'000'000, 101'000'000});
        const TraceSummary s = a.finish(1'000'000'000);

        QCOMPARE(s.unmatchedMessages, 0);
        QCOMPARE(s.seconds, 1);
        int r1Rows = 0;
        for (const BucketRow &r : s.rows[0]) {
            if (r.rank != 1)
                continue;
            ++r1Rows;
            QCOMPARE(r.commType, QStringLiteral("p2p"));
            QCOMPARE(r.recvBytes, qint64(1000));
            QCOMPARE(r.sendBytes, qint64(0));
            QCOMPARE(r.lateSender, 0.0); // send entered before the recv
        }
        QCOMPARE(r1Rows, 1);
    }

    void truncatedRecordsAreCountedNotLost()
    {
        TraceAggregator a;
        a.addSend({0, 1, 1, 3, 400, QStringLiteral("MPI_Send"), 10, 20});
        a.addRecv({2, 3, 1, 4, 300, QStringLiteral("MPI_Recv"), 1'500'000'000, 1'600'000'000});
        const TraceSummary s = a.finish(2'000'000'000);

        QCOMPARE(s.unmatchedMessages, 2);
        QCOMPARE(s.seconds, 2);
        QCOMPARE(s.rows[0].size(), 1);
        QCOMPARE(s.rows[0][0].rank, 0);
        QCOMPARE(s.rows[0][0].sendBytes, qint64(400));
        QCOMPARE(s.rows[1].size(), 1);
        QCOMPARE(s.rows[1][0].rank, 2);
        QCOMPARE(s.rows[1][0].recvBytes, qint64(300));
    }

    void classifiesFunctions()
    {
        QCOMPARE(classifyMpiFunction(QStringLiteral("MPI_Isend")), CallKind::P2P);
        QCOMPARE(classifyMpiFunction(QStringLiteral("MPI_Waitall")), CallKind::P2P);
        QCOMPARE(classifyMpiFunction(QStringLiteral("MPI_Iallreduce")), CallKind::Collective);
        QCOMPARE(classifyMpiFunction(QStringLiteral("MPI_Comm_rank")), CallKind::OtherMpi);
    }
};

QTEST_GUILESS_MAIN(TstTraceAggregator)
#include "tst_trace_aggregator.moc"
