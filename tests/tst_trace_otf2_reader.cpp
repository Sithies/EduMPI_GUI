#include <QtTest>

#include <otf2/otf2.h>

#include <cstdarg>
#include <thread>

#include "otf2_fixture.h"
#include "otf2_trace_reader.h"
#include "ring4_scenario.h"

using namespace trace;

class TstTraceOtf2Reader : public QObject
{
    Q_OBJECT

    static OTF2_ErrorCode sentinelHandler(void *, const char *, uint64_t, const char *,
                                          OTF2_ErrorCode code, const char *, va_list)
    {
        return code;
    }

    static bool readTrace(const QString &anchor, TraceSummary *s, TraceMeta *meta, QString *error)
    {
        TraceAggregator agg;
        if (!readOtf2(anchor, agg, meta, error))
            return false;
        *s = agg.finish(meta->traceLengthNs);
        return true;
    }

    static const DetailRow *findDetail(const QVector<DetailRow> &rows, const QString &fn, int rank,
                                       int partner)
    {
        for (const DetailRow &r : rows)
            if (r.function == fn && r.rank == rank && r.partner == partner)
                return &r;
        return nullptr;
    }

private slots:
    void readsRing4()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString anchor = writeRing4Otf2(tmp.filePath(QStringLiteral("ring4")));
        QVERIFY(!anchor.isEmpty());

        TraceMeta meta;
        TraceSummary s;
        QString error;
        QVERIFY2(readTrace(anchor, &s, &meta, &error), qPrintable(error));
        QCOMPARE(meta.traceLengthNs, qint64(2'500'000'000));
        QCOMPARE(meta.realtimeStart, QDateTime(QDate(2026, 1, 1), QTime(10, 0), QTimeZone::UTC));
        QCOMPARE(meta.name, QStringLiteral("ring4"));
        verifyRing4(s);
    }

    void readsWithoutRealtime()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.path(), false);
        QVERIFY(!anchor.isEmpty());
        TraceMeta meta;
        TraceSummary s;
        QString error;
        QVERIFY2(readTrace(anchor, &s, &meta, &error), qPrintable(error));
        QVERIFY(!meta.realtimeStart.isValid());
    }

    void mapsSubCommunicatorRanks()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.path());
        QVERIFY(!anchor.isEmpty());
        TraceMeta meta;
        TraceSummary s;
        QString error;
        QVERIFY2(readTrace(anchor, &s, &meta, &error), qPrintable(error));
        QVERIFY(s.details.size() >= 3);

        const DetailRow *send = findDetail(s.details[2], QStringLiteral("MPI_Send"), 3, 2);
        QVERIFY(send);
        QCOMPARE(send->commType, QStringLiteral("p2p"));
        QCOMPARE(send->sendBytes, qint64(200));
        const DetailRow *recv = findDetail(s.details[2], QStringLiteral("MPI_Recv"), 2, 3);
        QVERIFY(recv);
        QCOMPARE(recv->commType, QStringLiteral("p2p"));
        QCOMPARE(recv->recvBytes, qint64(200));
    }

    void readsTruncatedTrace()
    {
        QTemporaryDir tmp;
        const QString anchor = writeTruncatedOtf2(tmp.path());
        QVERIFY(!anchor.isEmpty());

        TraceAggregator agg;
        TraceMeta meta;
        QString error;
        QVERIFY2(readOtf2(anchor, agg, &meta, &error), qPrintable(error));
        const TraceSummary s = agg.finish(meta.traceLengthNs);
        QVERIFY(s.unmatchedMessages >= 1);
        QCOMPARE(s.rankCount, 4);
        QCOMPARE(meta.traceLengthNs, qint64(2'500'000'000));

        // R0's MPI_Recv [101 ms, open] is closed at trace end: its receive still counts.
        QVERIFY(!s.rows.isEmpty());
        const BucketRow *r0 = nullptr;
        for (const BucketRow &r : s.rows[0])
            if (r.rank == 0 && r.commType == QLatin1String("p2p"))
                r0 = &r;
        QVERIFY(r0);
        QCOMPARE(r0->recvBytes, qint64(1000));
        QVERIFY2(std::abs(r0->timeDiff - (0.001 + 2.399)) < 1e-6, qPrintable(QString::number(r0->timeDiff)));
    }

    void cancels()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.path());
        QVERIFY(!anchor.isEmpty());
        TraceAggregator agg;
        TraceMeta meta;
        QString error;
        const std::atomic_bool cancel{true};
        QVERIFY(!readOtf2(anchor, agg, &meta, &error, &cancel));
        QCOMPARE(error, QStringLiteral("Cancelled"));
    }

    void resolvesPaths()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.filePath(QStringLiteral("t")));
        QVERIFY(!anchor.isEmpty());
        const QString dir = QFileInfo(anchor).path();

        QString error;
        QCOMPARE(resolveTracePath(dir, &error), QDir(dir).filePath(QStringLiteral("traces.otf2")));
        QCOMPARE(resolveTracePath(anchor, &error), anchor);
        QCOMPARE(resolveTracePath(QUrl::fromLocalFile(anchor).toString(), &error), anchor);

        const QString empty = tmp.filePath(QStringLiteral("empty"));
        QVERIFY(QDir().mkpath(empty));
        error.clear();
        QVERIFY(resolveTracePath(empty, &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("No traces.otf2")), qPrintable(error));

        error.clear();
        const QString missing = tmp.filePath(QStringLiteral("missing.otf2"));
        QVERIFY(resolveTracePath(missing, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("File not found: %1").arg(missing));
    }

    void rejectsNonOtf2()
    {
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("traces.otf2"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not an OTF2 anchor file\n");
        f.close();

        TraceAggregator agg;
        TraceMeta meta;
        QString error;
        QVERIFY(!readOtf2(path, agg, &meta, &error));
        QVERIFY(!error.isEmpty());
        qInfo() << "error:" << error;
    }

    // Several trace windows may load at once on worker threads; the OTF2 error handler is
    // process-wide, so concurrent reads (successful and failing) must not interfere.
    void concurrentReads()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString anchor = writeRing4Otf2(tmp.filePath(QStringLiteral("ring4")));
        QVERIFY(!anchor.isEmpty());
        const QString bad = tmp.filePath(QStringLiteral("bad/traces.otf2"));
        QVERIFY(QDir().mkpath(QFileInfo(bad).path()));
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not an OTF2 anchor file\n");
        f.close();

        QString reference;
        {
            TraceAggregator agg;
            TraceMeta meta;
            QVERIFY(!readOtf2(bad, agg, &meta, &reference));
        }

        // The process-wide handler installed before the reads must be in place afterwards.
        const OTF2_ErrorCallback original = OTF2_Error_RegisterCallback(&sentinelHandler, nullptr);

        constexpr int kIterations = 50;
        struct Result {
            int ok = 0;
            int failed = 0;
            int correct = 0;
            int expectedError = 0;
            QString lastError;
        };
        auto reader = [&](const QString &path, Result *res) {
            for (int i = 0; i < kIterations; ++i) {
                TraceAggregator agg;
                TraceMeta meta;
                QString error;
                if (readOtf2(path, agg, &meta, &error)) {
                    ++res->ok;
                    const TraceSummary s = agg.finish(meta.traceLengthNs);
                    if (s.seconds == 3 && s.unmatchedMessages == 1 && s.rankCount == 4
                        && s.rows.value(0).size() == 4 && s.rows.value(1).size() == 4)
                        ++res->correct;
                } else {
                    ++res->failed;
                    res->lastError = error;
                    if (error == reference)
                        ++res->expectedError;
                }
            }
        };

        Result a, b, c;
        std::thread ta(reader, anchor, &a);
        std::thread tb(reader, anchor, &b);
        std::thread tc(reader, bad, &c);
        ta.join();
        tb.join();
        tc.join();

        QVERIFY2(a.failed == 0, qPrintable(a.lastError));
        QVERIFY2(b.failed == 0, qPrintable(b.lastError));
        QCOMPARE(a.correct, kIterations);
        QCOMPARE(b.correct, kIterations);
        const OTF2_ErrorCallback after = OTF2_Error_RegisterCallback(original, nullptr);
        QVERIFY(after == &sentinelHandler);

        QCOMPARE(c.failed, kIterations);
        QVERIFY2(c.expectedError == kIterations, qPrintable(c.lastError)); // captured, not lost

    }

    void rejectsMissingFile()
    {
        QTemporaryDir tmp;
        TraceAggregator agg;
        TraceMeta meta;
        QString error;
        QVERIFY(!readOtf2(tmp.filePath(QStringLiteral("nope.otf2")), agg, &meta, &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstTraceOtf2Reader)
#include "tst_trace_otf2_reader.moc"
