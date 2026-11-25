#include <QtTest>

#include "memory_budget.h"
#include "otf2_fixture.h"
#include "timeline_snapshot.h"
#include "trace_data_source.h"
#include "trace_timeline_model.h"

#include <QScopeGuard>
#include <QSettings>
#include <QThread>

using Matrix = QVector<QVector<long>>;

class TstTraceDataSource : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_anchor;
    QString m_many; // 8 ranks x 20000 messages, ~2 s
    QString m_dense; // 4 ranks x 5000 messages in the first 0.5 s, then 100 s idle

    static constexpr qint64 kMs = 1'000'000;

    static std::shared_ptr<const trace::TimelineStore> lastStore(const QSignalSpy &spy)
    {
        return spy.last().at(0).value<trace::TimelineSnapshot>().store;
    }

    static const DataColumn *findRow(const QList<DataColumn> &list, int rank, const QString &type)
    {
        for (const DataColumn &dc : list)
            if (dc.proc_rank == rank && dc.comm_type == type)
                return &dc;
        return nullptr;
    }

    static QList<DataColumn> lastList(const QSignalSpy &spy)
    {
        return spy.last().at(0).value<QList<DataColumn>>();
    }

    static Matrix lastMatrix(const QSignalSpy &spy) { return spy.last().at(0).value<Matrix>(); }
    static QVariantList p2pRow(const char *function, int rank, int partner, qlonglong sent,
                               qlonglong received)
    {
        return QVariantList{QString::fromLatin1(function), rank, partner, sent, received, QString()};
    }

    // Loads the ring4 fixture; QVERIFY inside, so callers check QTest::currentTestFailed().
    void load(Trace_Data_Source &src)
    {
        QSignalSpy ready(&src, &Trace_Data_Source::clusterComponentsReady);
        QSignalSpy failed(&src, &Trace_Data_Source::loadFailed);
        src.threadbuildClusterComponents();
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(0).toString()));
        QCOMPARE(ready.count(), 1);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_anchor = writeRing4Otf2(m_tmp.path());
        QVERIFY(!m_anchor.isEmpty());
        m_many = writeManyMessagesOtf2(m_tmp.filePath(QStringLiteral("many")), 8, 20000);
        QVERIFY(!m_many.isEmpty());
        m_dense = writeManyMessagesOtf2(m_tmp.filePath(QStringLiteral("dense")), 4, 5000, 100'000 * kMs);
        QVERIFY(!m_dense.isEmpty());
    }

    void cleanup() { qunsetenv("EDUMPI_TRACE_BUDGET_MB"); }

    // The UI's "Memory budget" field: QSettings ("EduMPI", "EduMPI_GUI") trace/memoryBudgetMB,
    // read by memoryBudgetBytes(). Restores the user's own setting afterwards.
    void memoryBudgetSettingRoundTrips()
    {
        QSettings settings(QStringLiteral("EduMPI"), QStringLiteral("EduMPI_GUI"));
        const QString key = QStringLiteral("trace/memoryBudgetMB");
        const QVariant saved = settings.value(key);
        auto restore = qScopeGuard([&] {
            QSettings s(QStringLiteral("EduMPI"), QStringLiteral("EduMPI_GUI"));
            if (saved.isValid())
                s.setValue(key, saved);
            else
                s.remove(key);
        });
        trace::setMemoryBudgetSettingMB(64);
        QCOMPARE(trace::memoryBudgetSettingMB(), 64);
        QCOMPARE(trace::memoryBudgetBytes(), quint64(64) * 1024 * 1024);
        trace::setMemoryBudgetSettingMB(-5);
        QCOMPARE(trace::memoryBudgetSettingMB(), 0);
        QVERIFY(trace::memoryBudgetBytes() >= 1); // automatic
    }

    void connectsAndClears()
    {
        Trace_Data_Source src(m_anchor);
        QSignalSpy connected(&src, &Trace_Data_Source::connectedToDB);
        QSignalSpy cleared(&src, &Trace_Data_Source::dbCleared);
        src.connectToDB();
        QCOMPARE(connected.count(), 1);
        QCOMPARE(connected.first().at(0).toBool(), true);
        src.clearDatabase();
        QCOMPARE(cleared.count(), 1);
    }

    void buildsComponents()
    {
        Trace_Data_Source src(QUrl::fromLocalFile(m_tmp.path()).toString()); // dir URL
        QSignalSpy ready(&src, &Trace_Data_Source::clusterComponentsReady);
        QSignalSpy info(&src, &Trace_Data_Source::traceInfoReady);
        QStringList order;
        connect(&src, &Trace_Data_Source::traceInfoReady, this, [&] { order << "info"; });
        connect(&src, &Trace_Data_Source::clusterComponentsReady, this, [&] { order << "components"; });

        src.threadbuildClusterComponents();

        QCOMPARE(ready.count(), 1);
        const auto map = ready.first().at(0).value<QMap<QString, QVector<int>>>();
        const QMap<QString, QVector<int>> expected{{"nodeA", {0, 1}}, {"nodeB", {2, 3}}};
        QCOMPARE(map, expected);

        QCOMPARE(info.count(), 1);
        QCOMPARE(info.first().at(0).toString(), QDir(m_tmp.path()).dirName());
        QCOMPARE(info.first().at(1).toInt(), 4);
        QCOMPARE(info.first().at(2).toDouble(), 2.5);
        QCOMPARE(info.first().at(3).toInt(), 1);
        QCOMPARE(order, (QStringList{"info", "components"}));
    }

    void startTimestampIsLocalRealtime()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        const QDateTime expected = QDateTime(QDate(2026, 1, 1), QTime(10, 0), QTimeZone::UTC).toLocalTime();
        QCOMPARE(src.startTimestamp(), expected);
        QCOMPARE(src.startTimestamp().timeSpec(), Qt::LocalTime);
    }

    void firstUpdateEmitsStartAndSecondZero()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy ts(&src, &Trace_Data_Source::setTimestamp);
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        QStringList order;
        connect(&src, &Trace_Data_Source::setTimestamp, this, [&] { order << "ts"; });
        connect(&src, &Trace_Data_Source::updateDataReady, this, [&] { order << "data"; });
        connect(&src, &Trace_Data_Source::setFunctionsString, this, [&] { order << "fn"; });
        connect(&src, &Trace_Data_Source::updateDetailedP2P, this, [&] { order << "p2p"; });
        connect(&src, &Trace_Data_Source::updateDetailedColl, this, [&] { order << "coll"; });
        connect(&src, &Trace_Data_Source::setCommMatrixP2PSend, this, [&] { order << "mP2PSend"; });
        connect(&src, &Trace_Data_Source::setCommMatrixP2PRecv, this, [&] { order << "mP2PRecv"; });
        connect(&src, &Trace_Data_Source::setCommMatrixCollSend, this, [&] { order << "mCollSend"; });
        connect(&src, &Trace_Data_Source::setCommMatrixCollRecv, this, [&] { order << "mCollRecv"; });
        connect(&src, &Trace_Data_Source::setCommMatrixTotalSend, this, [&] { order << "mTotal"; });

        src.updateData(0);

        QCOMPARE(ts.count(), 1);
        QCOMPARE(ts.first().at(0).toDateTime(), src.startTimestamp());
        QCOMPARE(ts.first().at(1).toBool(), true);
        QCOMPARE(order, (QStringList{"ts", "data", "fn", "p2p", "coll", "mP2PSend", "mP2PRecv",
                                     "mCollSend", "mCollRecv", "mTotal"}));

        const QList<DataColumn> list = lastList(data);
        QCOMPARE(list.size(), 4);
        for (const DataColumn &dc : list)
            QCOMPARE(dc.comm_type, QStringLiteral("p2p"));
        const DataColumn *r0 = findRow(list, 0, "p2p");
        QVERIFY(r0);
        QCOMPARE(r0->proc_name, QStringLiteral("nodeA"));
        QCOMPARE(r0->send_datasize, 1000L);
        QCOMPARE(r0->recv_datasize, 1000L);
        QVERIFY(qAbs(r0->late_sender - 0.299f) < 1e-5f);
        QVERIFY(qAbs(r0->time_diff - 0.302f) < 1e-5f);
        const DataColumn *r3 = findRow(list, 3, "p2p");
        QVERIFY(r3);
        QCOMPARE(r3->proc_name, QStringLiteral("nodeB"));

        // A second updateData does not emit the start timestamp again.
        src.updateData(0);
        QCOMPARE(ts.count(), 1);
    }

    void detailsOfSecondZero()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy fn(&src, &Trace_Data_Source::setFunctionsString);
        QSignalSpy p2p(&src, &Trace_Data_Source::updateDetailedP2P);
        QSignalSpy coll(&src, &Trace_Data_Source::updateDetailedColl);
        QSignalSpy pSend(&src, &Trace_Data_Source::setCommMatrixP2PSend);
        QSignalSpy pRecv(&src, &Trace_Data_Source::setCommMatrixP2PRecv);
        QSignalSpy cSend(&src, &Trace_Data_Source::setCommMatrixCollSend);
        QSignalSpy total(&src, &Trace_Data_Source::setCommMatrixTotalSend);

        src.updateData(0);

        const QString s = fn.last().at(0).toString();
        QVERIFY(s.contains("MPI_Send, "));
        QVERIFY(s.contains("MPI_Recv, "));
        QCOMPARE(s.size(), QString("MPI_Send, MPI_Recv, ").size());

        const auto p2pList = p2p.last().at(0).value<QList<QVariantList>>();
        // Same layout as Database_Thread::detailed_p2p_Query: function, processrank,
        // partnerrank, bytes sent, bytes received, communicator (Cores3D draws arrows from
        // the byte columns).
        QVERIFY(p2pList.contains(p2pRow("MPI_Send", 0, 1, 1000, 0)));
        QVERIFY(p2pList.contains(p2pRow("MPI_Recv", 0, 3, 0, 1000)));
        for (const QVariantList &row : p2pList)
            QCOMPARE(row.size(), 6);
        QVERIFY(coll.last().at(0).value<QList<QVariantList>>().isEmpty());

        const Matrix ps = lastMatrix(pSend);
        QCOMPARE(ps.size(), 4);
        for (const auto &row : ps)
            QCOMPARE(row.size(), 4);
        Matrix expectedSend(4, QVector<long>(4, 0));
        expectedSend[0][1] = expectedSend[1][2] = expectedSend[2][3] = expectedSend[3][0] = 1000;
        QCOMPARE(ps, expectedSend);
        Matrix expectedRecv(4, QVector<long>(4, 0));
        expectedRecv[1][0] = expectedRecv[2][1] = expectedRecv[3][2] = expectedRecv[0][3] = 1000;
        QCOMPARE(lastMatrix(pRecv), expectedRecv);
        QCOMPARE(lastMatrix(cSend), Matrix(4, QVector<long>(4, 0)));
        QCOMPARE(lastMatrix(total), expectedSend);
    }

    // Receives completed by MPI_Wait / MPI_Waitall must reach the p2p receive matrix and
    // appear as "MPI_Irecv" in the functions string (EduMPI DB books them on MPI_Irecv rows).
    void nonblockingReceivesFillRecvMatrix()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString anchor = writeNonblockingOtf2(tmp.path());
        QVERIFY(!anchor.isEmpty());
        Trace_Data_Source src(anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy fn(&src, &Trace_Data_Source::setFunctionsString);
        QSignalSpy p2p(&src, &Trace_Data_Source::updateDetailedP2P);
        QSignalSpy pSend(&src, &Trace_Data_Source::setCommMatrixP2PSend);
        QSignalSpy pRecv(&src, &Trace_Data_Source::setCommMatrixP2PRecv);
        QSignalSpy total(&src, &Trace_Data_Source::setCommMatrixTotalSend);

        src.updateData(0);

        const QString s = fn.last().at(0).toString();
        QVERIFY2(s.contains("MPI_Irecv, "), qPrintable(s));
        QVERIFY2(s.contains("MPI_Isend, "), qPrintable(s));
        QCOMPARE(s.size(), QString("MPI_Irecv, MPI_Isend, ").size());

        const auto p2pList = p2p.last().at(0).value<QList<QVariantList>>();
        QVERIFY(p2pList.contains(p2pRow("MPI_Irecv", 0, 1, 0, 400)));
        QVERIFY(p2pList.contains(p2pRow("MPI_Irecv", 1, 0, 0, 300)));
        QVERIFY(p2pList.contains(p2pRow("MPI_Isend", 0, 1, 300, 0)));
        QVERIFY(p2pList.contains(p2pRow("MPI_Isend", 1, 0, 400, 0)));

        Matrix expectedRecv(4, QVector<long>(4, 0));
        expectedRecv[0][1] = 400;
        expectedRecv[1][0] = 300;
        QCOMPARE(lastMatrix(pRecv), expectedRecv);
        Matrix expectedSend(4, QVector<long>(4, 0));
        expectedSend[0][1] = 300;
        expectedSend[1][0] = 400;
        QCOMPARE(lastMatrix(pSend), expectedSend);
        QCOMPARE(lastMatrix(total), expectedSend);
    }

    void advancesAndClampsToLastSecond()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        QSignalSpy pSend(&src, &Trace_Data_Source::setCommMatrixP2PSend);
        src.updateData(0); // second 0
        src.updateData(0); // second 1
        QList<DataColumn> list = lastList(data);
        QCOMPARE(list.size(), 4);
        for (const DataColumn &dc : list)
            QCOMPARE(dc.comm_type, QStringLiteral("collective"));
        src.updateData(0); // second 2
        src.updateData(0); // clamped to second 2
        list = lastList(data);
        const DataColumn *r1 = findRow(list, 1, "p2p");
        QVERIFY(r1);
        QCOMPARE(r1->send_datasize, 500L);
        QVERIFY(qAbs(r1->late_receiver - 0.300f) < 1e-5f);
        const Matrix ps = lastMatrix(pSend);
        QCOMPARE(ps[1][2], 500L);
        QCOMPARE(ps[3][2], 200L);
        QCOMPARE(ps[3][0], 100L);

        // reset goes back to second 0 without a new start timestamp
        QSignalSpy ts(&src, &Trace_Data_Source::setTimestamp);
        src.reset_actual_timestamp();
        src.updateData(0);
        QCOMPARE(ts.count(), 0);
        list = lastList(data);
        QCOMPARE(list.size(), 4);
        QCOMPARE(list.first().comm_type, QStringLiteral("p2p"));
    }

    void cumulativeDisplay()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        src.updateData(1);
        src.updateData(1);
        src.updateData(1);
        const QList<DataColumn> list = lastList(data);
        const DataColumn *r1 = findRow(list, 1, "p2p");
        QVERIFY(r1);
        QCOMPARE(r1->send_datasize, 1500L);
        QCOMPARE(r1->recv_datasize, 1000L);
        const DataColumn *r1c = findRow(list, 1, "collective");
        QVERIFY(r1c);
        QCOMPARE(r1c->send_datasize, 64L);
        const DataColumn *r2 = findRow(list, 2, "p2p");
        QVERIFY(r2);
        QCOMPARE(r2->recv_datasize, 1700L);
        QCOMPARE(list.size(), 8); // 4 ranks x {p2p, collective}
    }

    void periodQuery()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        QSignalSpy fn(&src, &Trace_Data_Source::setFunctionsString);
        QSignalSpy coll(&src, &Trace_Data_Source::updateDetailedColl);
        QSignalSpy cSend(&src, &Trace_Data_Source::setCommMatrixCollSend);
        QSignalSpy cRecv(&src, &Trace_Data_Source::setCommMatrixCollRecv);
        QSignalSpy total(&src, &Trace_Data_Source::setCommMatrixTotalSend);
        const QDateTime t = src.startTimestamp().addSecs(1);

        src.showDataFromTimePeriod(t, t);

        const QList<DataColumn> list = lastList(data);
        QCOMPARE(list.size(), 4);
        for (const DataColumn &dc : list)
            QCOMPARE(dc.comm_type, QStringLiteral("collective"));
        QCOMPARE(fn.last().at(0).toString(), QStringLiteral("MPI_Allreduce (), "));

        const auto collList = coll.last().at(0).value<QList<QVariantList>>();
        QCOMPARE(collList.size(), 4);
        for (const QVariantList &row : collList) {
            QCOMPARE(row.size(), 5); // + communicator, as in the DB rows
            QCOMPARE(row.at(4).toString(), QString());
            QCOMPARE(row.at(0).toString(), QStringLiteral("MPI_Allreduce"));
            QCOMPARE(row.at(2).toString(), QString());
            QVERIFY(!row.at(3).toByteArray().isEmpty());
        }

        const Matrix cs = lastMatrix(cSend);
        QCOMPARE(cs[0][1], 21L);
        Matrix expected(4, QVector<long>(4, 21));
        for (int i = 0; i < 4; ++i)
            expected[i][i] = 0;
        QCOMPARE(cs, expected);
        QCOMPARE(lastMatrix(cRecv), expected);
        QCOMPARE(lastMatrix(total), expected);
    }

    void periodQueryClampsAndSums()
    {
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        QSignalSpy total(&src, &Trace_Data_Source::setCommMatrixTotalSend);
        const QDateTime start = src.startTimestamp();

        src.showDataFromTimePeriod(start.addSecs(-10), start.addSecs(100));
        QCOMPARE(lastList(data).size(), 8);
        const Matrix tm = lastMatrix(total);
        QCOMPARE(tm[1][2], 1000L + 500L + 21L);
        QCOMPARE(tm[0][1], 1000L + 21L);

        src.showDataFromTimePeriod(start.addSecs(50), start.addSecs(60));
        QVERIFY(lastList(data).isEmpty());
        QCOMPARE(lastMatrix(total), Matrix(4, QVector<long>(4, 0)));
    }

    void fallbackStartTimeIsNonZero()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString anchor = writeRing4Otf2(tmp.path(), false);
        QVERIFY(!anchor.isEmpty());
        Trace_Data_Source src(anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(src.startTimestamp().time(), QTime(0, 0, 1));
        QCOMPARE(src.startTimestamp().date(), QDate::currentDate());
    }

    void reportsLoadError()
    {
        Trace_Data_Source src(m_tmp.filePath("does-not-exist/traces.otf2"));
        QSignalSpy ready(&src, &Trace_Data_Source::clusterComponentsReady);
        QSignalSpy failed(&src, &Trace_Data_Source::loadFailed);
        src.threadbuildClusterComponents();
        QCOMPARE(failed.count(), 1);
        QVERIFY(!failed.first().at(0).toString().isEmpty());
        QCOMPARE(ready.count(), 0);

        // queries on an unloaded source emit nothing and do not crash
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        src.updateData(0);
        src.showDataFromTimePeriod(QDateTime::currentDateTime(), QDateTime::currentDateTime());
        QCOMPARE(data.count(), 0);
    }

    void cancelledLoadFails()
    {
        Trace_Data_Source src(m_anchor);
        QSignalSpy ready(&src, &Trace_Data_Source::clusterComponentsReady);
        QSignalSpy failed(&src, &Trace_Data_Source::loadFailed);
        src.set_thread_running(false);
        src.threadbuildClusterComponents();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
    }
    // ---- Stage 2: memory budget, detail requests, window queries -------------------------

    void fullModeDeliversCompleteStore()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "256");
        Trace_Data_Source src(m_anchor);
        QStringList order;
        QSignalSpy timeline(&src, &Trace_Data_Source::timelineReady);
        connect(&src, &Trace_Data_Source::timelineReady, this, [&] { order << "timeline"; });
        connect(&src, &Trace_Data_Source::clusterComponentsReady, this, [&] { order << "components"; });
        load(src);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(order, (QStringList{"timeline", "components"}));
        const auto store = lastStore(timeline);
        QVERIFY(store);
        QVERIFY(store->complete);
        QCOMPARE(store->rows.size(), 4);
        QVERIFY(store->pyramid.levels >= 1);
        // Full mode ignores detail requests.
        src.requestDetail(100 * kMs, 200 * kMs);
        QCOMPARE(timeline.count(), 1);
    }

    void smallBudgetSwitchesToWindowedMode()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        Trace_Data_Source src(m_many);
        QSignalSpy timeline(&src, &Trace_Data_Source::timelineReady);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(timeline.count(), 1);
        const auto store = lastStore(timeline);
        QVERIFY(store);
        QVERIFY(!store->complete);
        QCOMPARE(store->rows.size(), 8);
        QVERIFY(store->pyramid.levels >= 1);
        QVERIFY(trace::pyramidBytes(8, store->pyramid.levels) <= 512 * 1024);
        QVERIFY2(store->bytesUsed() <= 1024 * 1024, qPrintable(QString::number(store->bytesUsed())));
        for (const trace::RankRow &r : store->rows)
            QVERIFY(r.activities.isEmpty());
        // The overview carries the detail-window cap.
        const qint64 cap = timeline.last().at(0).value<trace::TimelineSnapshot>().maxDetailWindowNs;
        QVERIFY(cap > 0);
        QVERIFY(cap < store->traceLengthNs);
    }

    // A stopped source still answers a detail request (null store), so that the model's
    // loadingDetail does not stay true.
    void detailRequestAfterStopIsAnswered()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        Trace_Data_Source src(m_many);
        QSignalSpy timeline(&src, &Trace_Data_Source::timelineReady);
        load(src);
        if (QTest::currentTestFailed())
            return;
        src.set_thread_running(false);
        src.requestDetail(1000 * kMs, 1001 * kMs);
        QCOMPARE(timeline.count(), 2);
        const trace::TimelineSnapshot snap = timeline.last().at(0).value<trace::TimelineSnapshot>();
        QVERIFY(!snap.store);
        QCOMPARE(snap.requestFromNs, 1000 * kMs);
        QCOMPARE(snap.requestToNs, 1001 * kMs);
    }

    void detailRequestLoadsOnlyWindow()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        Trace_Data_Source src(m_many);
        QSignalSpy timeline(&src, &Trace_Data_Source::timelineReady);
        load(src);
        if (QTest::currentTestFailed())
            return;
        const qint64 a = 1000 * kMs;
        const qint64 b = a + kMs;
        src.requestDetail(a, b);
        QCOMPARE(timeline.count(), 2);
        const trace::TimelineSnapshot snap = timeline.last().at(0).value<trace::TimelineSnapshot>();
        QCOMPARE(snap.requestFromNs, a);
        QCOMPARE(snap.requestToNs, b);
        const auto store = snap.store;
        QVERIFY(store);
        QVERIFY(!store->complete);
        QVERIFY(store->fromNs <= a);
        QVERIFY(store->toNs >= b);
        QVERIFY(store->toNs - store->fromNs < 100 * kMs); // a window, not the whole run
        int activities = 0;
        for (const trace::RankRow &r : store->rows) {
            for (const trace::Activity &act : r.activities) {
                QVERIFY(act.endNs >= store->fromNs && act.startNs <= store->toNs);
                ++activities;
            }
        }
        QVERIFY(activities > 0);
        QVERIFY(store->bytesUsed() <= 256 * 1024);

        // The loaded window answers window queries exactly.
        QSignalSpy shown(&src, &Trace_Data_Source::traceWindowShown);
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        src.showTraceWindow(a, b);
        QCOMPARE(shown.count(), 1);
        QCOMPARE(shown.last().at(2).toBool(), true);
        QCOMPARE(lastList(data).size(), 8);
    }

    // Requests from another thread while the source works on its own thread: only the latest
    // one is answered last.
    void latestDetailRequestWins()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        QThread worker;
        auto *src = new Trace_Data_Source(m_many);
        src->moveToThread(&worker);
        QSignalSpy timeline(src, &Trace_Data_Source::timelineReady);
        QSignalSpy ready(src, &Trace_Data_Source::clusterComponentsReady);
        worker.start();
        QMetaObject::invokeMethod(src, &Trace_Data_Source::threadbuildClusterComponents, Qt::QueuedConnection);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
        src->requestDetail(500 * kMs, 501 * kMs);
        src->requestDetail(600 * kMs, 601 * kMs);
        src->requestDetail(1500 * kMs, 1501 * kMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline.count() >= 2
                                     && timeline.last().at(0).value<trace::TimelineSnapshot>().requestFromNs
                                            == 1500 * kMs,
                                 20000);
        QTest::qWait(300);
        const trace::TimelineSnapshot snap = timeline.last().at(0).value<trace::TimelineSnapshot>();
        QCOMPARE(snap.requestFromNs, 1500 * kMs);
        QVERIFY(snap.store && snap.store->fromNs <= 1500 * kMs && snap.store->toNs >= 1501 * kMs);
        src->set_thread_running(false);
        worker.quit();
        worker.wait();
        delete src;
    }

    void showTraceWindowExact()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "256");
        Trace_Data_Source src(m_anchor);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        QSignalSpy pSend(&src, &Trace_Data_Source::setCommMatrixP2PSend);
        QSignalSpy shown(&src, &Trace_Data_Source::traceWindowShown);
        src.showTraceWindow(0, 300 * kMs);
        QCOMPARE(shown.count(), 1);
        QCOMPARE(shown.last().at(0).toLongLong(), 0);
        QCOMPARE(shown.last().at(1).toLongLong(), 300 * kMs);
        QCOMPARE(shown.last().at(2).toBool(), true);

        const QList<DataColumn> list = lastList(data);
        const DataColumn *r0 = findRow(list, 0, "p2p");
        const DataColumn *r3 = findRow(list, 3, "p2p");
        QVERIFY(r0 && r3);
        QCOMPARE(r0->proc_name, QStringLiteral("nodeA"));
        QCOMPARE(r0->send_datasize, 1000L);
        QCOMPARE(r0->recv_datasize, 1000L);
        QVERIFY(qAbs(r0->late_sender - 0.299f) < 1e-5f);
        QCOMPARE(r3->send_datasize, 0L);
        QCOMPARE(r3->recv_datasize, 1000L);
        // R3's send at 400 ms is outside the window.
        Matrix expectedSend(4, QVector<long>(4, 0));
        expectedSend[0][1] = expectedSend[1][2] = expectedSend[2][3] = 1000;
        QCOMPARE(lastMatrix(pSend), expectedSend);
    }

    // maxDetailWindowNs assumes evenly spread events; in a dense phase a window of that length
    // holds far more. The detail build must still stay within budget / 4, the request must be
    // answered (loadingDetail back to false) and the model must not re-request in a loop.
    void denseDetailWindowStaysWithinBudget()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        const quint64 cap = 1024 * 1024 / 4;
        Trace_Data_Source src(m_dense);
        QSignalSpy timeline(&src, &Trace_Data_Source::timelineReady);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(timeline.count(), 1);
        const trace::TimelineSnapshot overview = timeline.last().at(0).value<trace::TimelineSnapshot>();
        QVERIFY(overview.store && !overview.store->complete);
        // The even-spread cap admits a window holding the whole dense phase.
        QVERIFY2(overview.maxDetailWindowNs >= 1000 * kMs, qPrintable(QString::number(overview.maxDetailWindowNs)));

        TraceTimelineModel model;
        model.addSnapshot(overview);
        connect(&model, &TraceTimelineModel::detailRequested, &src, &Trace_Data_Source::requestDetail,
                Qt::DirectConnection);
        connect(&src, &Trace_Data_Source::timelineReady, &model, &TraceTimelineModel::addSnapshot,
                Qt::DirectConnection);
        const qint64 from = 100 * kMs;
        const qint64 to = 400 * kMs;
        QVERIFY(model.requestDetail(from, to));
        QTRY_COMPARE_WITH_TIMEOUT(timeline.count(), 2, 20000);
        QVERIFY(!model.loadingDetail());
        const trace::TimelineSnapshot snap = timeline.last().at(0).value<trace::TimelineSnapshot>();
        QCOMPARE(snap.requestFromNs, from);
        QCOMPARE(snap.requestToNs, to);
        // The dense phase does not fit: refused as over budget (not a read failure).
        QVERIFY(!snap.store);
        QVERIFY(snap.overBudget);

        // Asking again for the same range is not re-forwarded (no load loop).
        QVERIFY(model.requestDetail(from, to));
        QTest::qWait(TraceTimelineModel::kDebounceMs + 200);
        QCOMPARE(timeline.count(), 2);
        QVERIFY(!model.loadingDetail());

        // Zooming in: a short request inside the dense phase is forwarded by the model and gets
        // a (within-budget) window.
        QVERIFY(model.requestDetail(200 * kMs, 201 * kMs));
        QTRY_COMPARE_WITH_TIMEOUT(timeline.count(), 3, 20000);
        QVERIFY(!model.loadingDetail());
        const auto small = timeline.last().at(0).value<trace::TimelineSnapshot>().store;
        QVERIFY(small);
        QVERIFY2(small->bytesUsed() <= cap, qPrintable(QString::number(small->bytesUsed())));
        QVERIFY(small->fromNs <= 200 * kMs && small->toNs >= 201 * kMs);
    }

    void showTraceWindowFallsBackToSeconds()
    {
        qputenv("EDUMPI_TRACE_BUDGET_MB", "1");
        Trace_Data_Source src(m_many);
        load(src);
        if (QTest::currentTestFailed())
            return;
        QSignalSpy data(&src, &Trace_Data_Source::updateDataReady);
        QSignalSpy shown(&src, &Trace_Data_Source::traceWindowShown);
        QSignalSpy total(&src, &Trace_Data_Source::setCommMatrixTotalSend);

        src.showTraceWindow(250 * kMs, 1500 * kMs); // nothing loaded in detail
        QCOMPARE(shown.count(), 1);
        QCOMPARE(shown.last().at(0).toLongLong(), 0);
        QCOMPARE(shown.last().at(1).toLongLong(), 2000 * kMs);
        QCOMPARE(shown.last().at(2).toBool(), false);
        const QList<DataColumn> windowRows = lastList(data);
        const Matrix windowTotal = lastMatrix(total);

        // Same rows as the stage-1 query of seconds 0..1.
        const QDateTime start = src.startTimestamp();
        src.showDataFromTimePeriod(start, start.addSecs(1));
        const QList<DataColumn> secondRows = lastList(data);
        QCOMPARE(windowRows.size(), secondRows.size());
        QCOMPARE(windowRows.size(), 8);
        for (int i = 0; i < windowRows.size(); ++i) {
            QCOMPARE(windowRows[i].proc_rank, secondRows[i].proc_rank);
            QCOMPARE(windowRows[i].send_datasize, secondRows[i].send_datasize);
            QCOMPARE(windowRows[i].recv_datasize, secondRows[i].recv_datasize);
        }
        QCOMPARE(windowTotal, lastMatrix(total));

        // A detail window elsewhere does not make the query exact either.
        src.requestDetail(1800 * kMs, 1801 * kMs);
        src.showTraceWindow(250 * kMs, 260 * kMs);
        QCOMPARE(shown.last().at(2).toBool(), false);
        QCOMPARE(shown.last().at(0).toLongLong(), 0);
        QCOMPARE(shown.last().at(1).toLongLong(), 1000 * kMs);
    }
};

QTEST_GUILESS_MAIN(TstTraceDataSource)
#include "tst_trace_data_source.moc"
