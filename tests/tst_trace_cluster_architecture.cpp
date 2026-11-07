#include <QtTest>

#include "cluster_architecture.h"
#include "otf2_fixture.h"

// Cluster_Architecture fed by Trace_Data_Source via startTraceSource (ring4 fixture).
class TstTraceClusterArchitecture : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_anchor;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_anchor = writeRing4Otf2(m_tmp.path());
        QVERIFY(!m_anchor.isEmpty());
    }

    void loadsTraceAndShowsSecondZero()
    {
        Cluster_Architecture ca;
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QSignalSpy info(&ca, &Cluster_Architecture::traceInfoReady);
        QSignalSpy dataIn(&ca, &Cluster_Architecture::dataIn);
        QSignalSpy failed(&ca, &Cluster_Architecture::traceLoadFailed);

        QVERIFY(ca.startTraceSource(m_anchor));
        // A second source cannot be started while one is running.
        QVERIFY(!ca.startTraceSource(m_anchor));

        QTRY_COMPARE(built.count(), 1);
        QVERIFY(failed.isEmpty());

        QCOMPARE(info.count(), 1);
        QCOMPARE(info.first().at(1).toInt(), 4);
        QCOMPARE(info.first().at(2).toDouble(), 2.5);
        QCOMPARE(info.first().at(3).toInt(), 1);
        QCOMPARE(ca.proc_num(), 4);

        QCOMPARE(ca.count(), 2);
        QCOMPARE(ca.nodeAt(0)->getName(), QStringLiteral("nodeA"));
        QCOMPARE(ca.nodeAt(1)->getName(), QStringLiteral("nodeB"));

        // Start timestamp reaches the timeline via setTimestamp -> handleTimestamp -> dataIn.
        QTRY_VERIFY(!dataIn.isEmpty());
        QCOMPARE(dataIn.first().at(2).toBool(), true);

        // Rank values of second 0 (first updateData after the components are built).
        Cluster_Rank *r0 = ca.nodeAt(0)->rankAt(0);
        QVERIFY(r0);
        QTRY_VERIFY(qAbs(r0->p2p_late_sender() - 0.299f) < 1e-5f);
        QCOMPARE(r0->p2p_send_datasize(), 1000L);
        QCOMPARE(r0->p2p_recv_datasize(), 1000L);
        QVERIFY(qAbs(r0->p2p_timediff() - 0.302f) < 1e-5f);

        // Reset stops and deletes the trace source; a new trace can be started afterwards.
        ca.resetCluster_Architecture();
        QCOMPARE(ca.count(), 0);
        QSignalSpy built2(&ca, &Cluster_Architecture::componentsBuilt);
        QVERIFY(ca.startTraceSource(m_anchor));
        QTRY_COMPARE(built2.count(), 1);
    }

    // Checks a freshly started second session: exactly one build, no failure, ring4 values,
    // and no stale delivery of the first session arriving afterwards.
    void checkSecondSession(Cluster_Architecture &ca)
    {
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QSignalSpy info(&ca, &Cluster_Architecture::traceInfoReady);
        QSignalSpy failed(&ca, &Cluster_Architecture::traceLoadFailed);

        QVERIFY(ca.startTraceSource(m_anchor));
        QTRY_COMPARE(built.count(), 1);
        Cluster_Rank *r0 = ca.nodeAt(0)->rankAt(0);
        QVERIFY(r0);
        QTRY_VERIFY(qAbs(r0->p2p_late_sender() - 0.299f) < 1e-5f);

        QTest::qWait(200); // give stale events a chance to show up
        QCOMPARE(built.count(), 1);
        QCOMPARE(info.count(), 1);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(0).toString()));
        QCOMPARE(ca.count(), 2);
        QCOMPARE(ca.nodeAt(0)->ranks().size(), 2);
        QCOMPARE(r0->p2p_send_datasize(), 1000L);
    }

    // Reset right after start: the first load is cancelled or not yet started.
    void resetImmediatelyAfterStart()
    {
        Cluster_Architecture ca;
        QSignalSpy failed(&ca, &Cluster_Architecture::traceLoadFailed);
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QVERIFY(ca.startTraceSource(m_anchor));
        ca.resetCluster_Architecture();
        checkSecondSession(ca);
        if (QTest::currentTestFailed())
            return;
        QVERIFY(failed.isEmpty());
        QCOMPARE(built.count(), 1);
    }

    // Reset after the first load finished but before the GUI thread processed its results:
    // its clusterComponentsReady / traceInfoReady / data are queued and must be dropped.
    void resetWithQueuedResults()
    {
        Cluster_Architecture ca;
        QSignalSpy failed(&ca, &Cluster_Architecture::traceLoadFailed);
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QVERIFY(ca.startTraceSource(m_anchor));
        QThread::msleep(300); // worker loads the tiny fixture; GUI events are not processed
        ca.resetCluster_Architecture();
        checkSecondSession(ca);
        if (QTest::currentTestFailed())
            return;
        QVERIFY(failed.isEmpty());
        QCOMPARE(built.count(), 1);
    }

    // set_end_time(-1) cancels the trace source; the resulting "Cancelled" is not reported.
    void ownCancelIsNotReported()
    {
        Cluster_Architecture ca;
        QSignalSpy failed(&ca, &Cluster_Architecture::traceLoadFailed);
        QVERIFY(ca.startTraceSource(m_anchor));
        ca.set_end_time(-1);
        QTest::qWait(300);
        QVERIFY(failed.isEmpty());
    }

    void reportsLoadFailure()
    {
        Cluster_Architecture ca;
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QSignalSpy failed(&ca, &Cluster_Architecture::traceLoadFailed);
        QVERIFY(ca.startTraceSource(m_tmp.filePath("does-not-exist.otf2")));
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(!failed.first().at(0).toString().isEmpty());
        QCOMPARE(built.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TstTraceClusterArchitecture)
#include "tst_trace_cluster_architecture.moc"
