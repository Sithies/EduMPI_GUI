#include <QtTest>

#include "cluster_architecture.h"
#include "otf2_fixture.h"
#include "trace_timeline_model.h"

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
        QSignalSpy progress(&ca, &Cluster_Architecture::traceLoadProgress);
        QSignalSpy memory(&ca, &Cluster_Architecture::traceMemoryInfo);

        QVERIFY(ca.startTraceSource(m_anchor));
        // A second source cannot be started while one is running.
        QVERIFY(!ca.startTraceSource(m_anchor));

        QTRY_COMPARE(built.count(), 1);
        QVERIFY(failed.isEmpty());

        // The load reported its progress up to 100 % before the components were built.
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(0).toInt(), 100);
        // Budget / mode of the load reach QML (the ring4 trace fits any budget).
        QCOMPARE(memory.count(), 1);
        QVERIFY(memory.first().at(0).toLongLong() > 0);
        QVERIFY(!memory.first().at(1).toString().isEmpty());
        QCOMPARE(memory.first().at(3).toBool(), true);

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
    void timelinePropertyAfterLoad()
    {
        Cluster_Architecture ca;
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QSignalSpy timelineChanged(&ca, &Cluster_Architecture::timelineChanged);
        QVERIFY(ca.startTraceSource(m_anchor));
        QTRY_COMPARE(built.count(), 1);
        QObject *timeline = ca.property("timeline").value<QObject *>();
        QVERIFY(timeline);
        QVERIFY(!timelineChanged.isEmpty());
        QCOMPARE(timeline->property("rankCount").toInt(), 4);
        QCOMPARE(timeline->property("traceLengthNs").toLongLong(), 2500LL * 1000 * 1000);
        QCOMPARE(timeline->property("windowed").toBool(), false);
        auto *model = qobject_cast<TraceTimelineModel *>(timeline);
        QVERIFY(model);
        QVERIFY(model->overview());
        QCOMPARE(model->detail(), model->overview()); // full mode: the complete store

        // Window queries reach the source and come back in seconds.
        QSignalSpy shown(&ca, &Cluster_Architecture::traceWindowShown);
        ca.showTraceWindow(0.0, 0.3);
        QTRY_COMPARE(shown.count(), 1);
        QCOMPARE(shown.first().at(0).toDouble(), 0.0);
        QCOMPARE(shown.first().at(1).toDouble(), 0.3);
        QCOMPARE(shown.first().at(2).toBool(), true);

        ca.resetCluster_Architecture();
        QVERIFY(!ca.property("timeline").value<QObject *>());
    }

    void timelineDetailRequestsAreDebounced()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString many = writeManyMessagesOtf2(tmp.path(), 8, 20000);
        QVERIFY(!many.isEmpty());
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        Cluster_Architecture ca;
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QVERIFY(ca.startTraceSource(many));
        QTRY_COMPARE_WITH_TIMEOUT(built.count(), 1, 20000);
        qunsetenv("EDUMPI_TRACE_BUDGET_MB");
        auto *model = qobject_cast<TraceTimelineModel *>(ca.timeline());
        QVERIFY(model);
        QVERIFY(model->windowed());
        QVERIFY(model->overview());
        QVERIFY(!model->detail());

        QSignalSpy forwarded(model, &TraceTimelineModel::detailRequested);
        // Loading state at the moment of forwarding: the (early-stopping) read can be answered
        // before QTRY_COMPARE below returns, so it is not checked afterwards.
        bool loadingWhenForwarded = false;
        connect(model, &TraceTimelineModel::detailRequested, this,
                [&] { loadingWhenForwarded = model->loadingDetail(); });
        const qint64 ms = 1000 * 1000;
        model->requestDetail(100 * ms, 101 * ms);
        model->requestDetail(200 * ms, 201 * ms);
        model->requestDetail(300 * ms, 301 * ms);
        QCOMPARE(forwarded.count(), 0); // debounced
        QTRY_COMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.first().at(0).toLongLong(), 300 * ms);
        QVERIFY(loadingWhenForwarded);
        QTRY_VERIFY_WITH_TIMEOUT(!model->loadingDetail(), 20000);
        const auto detail = model->detail();
        QVERIFY(detail);
        QVERIFY(detail->fromNs <= 300 * ms && detail->toNs >= 301 * ms);

        // Moving away from the loaded window: no matching detail until the new one arrives.
        model->requestDetail(1500 * ms, 1501 * ms);
        QVERIFY(!model->detail());
        QTRY_VERIFY_WITH_TIMEOUT(model->detail() && model->detail()->fromNs <= 1500 * ms, 20000);
    }
    // Model in windowed mode with a synthetic overview (10 s run, detail cap 1 s).
    static void setUpWindowedModel(TraceTimelineModel &model)
    {
        auto store = std::make_shared<trace::TimelineStore>();
        store->traceLengthNs = 10'000'000'000;
        store->toNs = store->traceLengthNs;
        store->complete = false;
        trace::TimelineSnapshot snap;
        snap.store = store;
        snap.maxDetailWindowNs = 1'000'000'000;
        model.addSnapshot(snap);
    }

    void modelIgnoresRequestsWiderThanCap()
    {
        TraceTimelineModel model;
        setUpWindowedModel(model);
        QVERIFY(model.windowed());
        QCOMPARE(model.maxDetailWindowNs(), 1'000'000'000LL);
        QCOMPARE(model.property("maxDetailWindowNs").toLongLong(), 1'000'000'000LL);
        QSignalSpy forwarded(&model, &TraceTimelineModel::detailRequested);
        const qint64 ms = 1000 * 1000;
        QVERIFY(!model.requestDetail(0, 2000 * ms));
        QTest::qWait(TraceTimelineModel::kDebounceMs + 150);
        QCOMPARE(forwarded.count(), 0);
        QVERIFY(!model.loadingDetail());
        QVERIFY(model.requestDetail(0, 500 * ms));
        QTRY_COMPARE(forwarded.count(), 1);
    }

    void modelDoesNotReforwardUnansweredRange()
    {
        TraceTimelineModel model;
        setUpWindowedModel(model);
        QSignalSpy forwarded(&model, &TraceTimelineModel::detailRequested);
        const qint64 ms = 1000 * 1000;
        const auto wait = [] { QTest::qWait(TraceTimelineModel::kDebounceMs + 150); };

        // Failed read (null store).
        QVERIFY(model.requestDetail(1000 * ms, 1500 * ms));
        QTRY_COMPARE(forwarded.count(), 1);
        model.addSnapshot({nullptr, 1000 * ms, 1500 * ms});
        QVERIFY(!model.loadingDetail());
        model.requestDetail(1000 * ms, 1500 * ms); // identical
        wait();
        QCOMPARE(forwarded.count(), 1);
        model.requestDetail(1100 * ms, 1400 * ms); // contained
        wait();
        QCOMPARE(forwarded.count(), 1);

        // Answer that does not cover the request.
        model.requestDetail(3000 * ms, 3500 * ms);
        QTRY_COMPARE(forwarded.count(), 2);
        auto narrow = std::make_shared<trace::TimelineStore>();
        narrow->fromNs = 3200 * ms;
        narrow->toNs = 3300 * ms;
        model.addSnapshot({narrow, 3000 * ms, 3500 * ms});
        QVERIFY(!model.loadingDetail());
        QVERIFY(!model.detail());
        model.requestDetail(3000 * ms, 3500 * ms);
        wait();
        QCOMPARE(forwarded.count(), 2);

        // A different range is forwarded again.
        model.requestDetail(5000 * ms, 5500 * ms);
        QTRY_COMPARE(forwarded.count(), 3);
    }

    // Over the memory budget (dense phase): zooming in further must load details again once
    // the range is at most half the one that did not fit; each retry halves (no load loop).
    void modelRetriesNarrowerAfterOverBudget()
    {
        TraceTimelineModel model;
        setUpWindowedModel(model);
        QSignalSpy forwarded(&model, &TraceTimelineModel::detailRequested);
        const qint64 ms = 1000 * 1000;
        const auto wait = [] { QTest::qWait(TraceTimelineModel::kDebounceMs + 150); };
        const auto overBudget = [](qint64 from, qint64 to) {
            trace::TimelineSnapshot s{nullptr, from, to};
            s.overBudget = true;
            return s;
        };

        QVERIFY(model.requestDetail(1000 * ms, 2000 * ms));
        QTRY_COMPARE(forwarded.count(), 1);
        model.addSnapshot(overBudget(1000 * ms, 2000 * ms));
        QVERIFY(!model.loadingDetail());
        QVERIFY(model.detailOverBudget()); // the view shows "zoom in further"
        QVERIFY(model.property("detailOverBudget").toBool());

        model.requestDetail(1100 * ms, 1800 * ms); // 70 %: still blocked
        QVERIFY(model.detailOverBudget());
        wait();
        QCOMPARE(forwarded.count(), 1);
        model.requestDetail(1200 * ms, 1700 * ms); // half: forwarded
        QVERIFY(!model.detailOverBudget()); // will be tried: no note meanwhile
        QTRY_COMPARE(forwarded.count(), 2);
        model.addSnapshot(overBudget(1200 * ms, 1700 * ms));
        QVERIFY(!model.loadingDetail());

        model.requestDetail(1250 * ms, 1650 * ms); // 80 % of the new reference: blocked
        wait();
        QCOMPARE(forwarded.count(), 2);
        model.requestDetail(1300 * ms, 1500 * ms); // 40 %: forwarded
        QTRY_COMPARE(forwarded.count(), 3);
        auto fits = std::make_shared<trace::TimelineStore>();
        fits->fromNs = 1300 * ms;
        fits->toNs = 1500 * ms;
        model.addSnapshot({fits, 1300 * ms, 1500 * ms});
        QVERIFY(!model.loadingDetail());
        QVERIFY(model.detail());
        QVERIFY(!model.detailOverBudget());

        // A failure that is not over budget still blocks contained ranges of any size.
        model.requestDetail(4000 * ms, 5000 * ms);
        QTRY_COMPARE(forwarded.count(), 4);
        model.addSnapshot({nullptr, 4000 * ms, 5000 * ms});
        model.requestDetail(4100 * ms, 4200 * ms);
        wait();
        QCOMPARE(forwarded.count(), 4);
        QVERIFY(!model.detailOverBudget()); // a read failure is not "too dense"

        // Leaving an over-budget range clears the note.
        model.requestDetail(6000 * ms, 7000 * ms);
        QTRY_COMPARE(forwarded.count(), 5);
        model.addSnapshot(overBudget(6000 * ms, 7000 * ms));
        QVERIFY(model.detailOverBudget());
        model.requestDetail(8000 * ms, 8500 * ms);
        QVERIFY(!model.detailOverBudget());
    }

    // set_end_time(-1) stops the source but keeps the model: a later detail request must not
    // leave loadingDetail stuck at true.
    void loadingDetailClearsAfterCancel()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString many = writeManyMessagesOtf2(tmp.path(), 8, 20000);
        QVERIFY(!many.isEmpty());
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        Cluster_Architecture ca;
        QSignalSpy built(&ca, &Cluster_Architecture::componentsBuilt);
        QVERIFY(ca.startTraceSource(many));
        QTRY_COMPARE_WITH_TIMEOUT(built.count(), 1, 20000);
        qunsetenv("EDUMPI_TRACE_BUDGET_MB");
        auto *model = qobject_cast<TraceTimelineModel *>(ca.timeline());
        QVERIFY(model && model->windowed());
        ca.set_end_time(-1);
        const qint64 ms = 1000 * 1000;
        QSignalSpy forwarded(model, &TraceTimelineModel::detailRequested);
        QVERIFY(model->requestDetail(700 * ms, 701 * ms));
        QTRY_COMPARE(forwarded.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!model->loadingDetail(), 5000);
        QVERIFY(!model->detail());
    }
};

QTEST_GUILESS_MAIN(TstTraceClusterArchitecture)
#include "tst_trace_cluster_architecture.moc"
