#include <QtTest>

#include "otf2_fixture.h"
#include "trace_data_source.h"

using Matrix = QVector<QVector<long>>;

class TstTraceDataSource : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_anchor;

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
        QVERIFY(p2pList.contains(QVariantList{QString("MPI_Send"), 0, 1}));
        QVERIFY(p2pList.contains(QVariantList{QString("MPI_Recv"), 0, 3}));
        for (const QVariantList &row : p2pList)
            QCOMPARE(row.size(), 3);
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
            QCOMPARE(row.size(), 4);
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
};

QTEST_GUILESS_MAIN(TstTraceDataSource)
#include "tst_trace_data_source.moc"
