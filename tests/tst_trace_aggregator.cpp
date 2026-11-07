#include <QtTest>

#include "ring4_scenario.h"
#include "trace_aggregator.h"

using namespace trace;

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
