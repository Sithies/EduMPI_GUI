#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <limits>
#include <tuple>

#include "otf2_fixture.h"
#include "otf2_trace_reader.h"
#include "timeline_builder.h"
#include "trace_aggregator.h"

using namespace trace;

namespace {

constexpr qint64 kMs = 1'000'000;
constexpr qint64 kAll = std::numeric_limits<qint64>::max();

struct Built {
    std::shared_ptr<TimelineStore> store;
    TraceMeta meta;
};

Built build(const QString &anchor, qint64 from = 0, qint64 to = kAll, bool keep = true, int levels = 4)
{
    Built b;
    TraceAggregator agg;
    TimelineBuilder builder(agg, from, to, keep, levels);
    QString error;
    if (!readOtf2(anchor, builder, &b.meta, &error)) {
        qWarning("%s", qPrintable(error));
        return b;
    }
    agg.finish(b.meta.traceLengthNs);
    b.store = builder.finish();
    return b;
}

const RankRow &row(const TimelineStore &s, int rank)
{
    return s.rows[s.rowIndexOfRank(rank)];
}

const Activity *findActivity(const TimelineStore &s, int rank, const QString &fn, qint64 startNs)
{
    for (const Activity &a : row(s, rank).activities)
        if (s.functions.value(a.function) == fn && a.startNs == startNs)
            return &a;
    return nullptr;
}

QVector<int> linksOf(const TimelineStore &s, int rank, const Activity &a)
{
    const RankRow &r = row(s, rank);
    QVector<int> out;
    for (int i = 0; i < a.linkCount; ++i)
        out.append(r.links[a.firstLink + i]);
    return out;
}

const Message *findMessage(const TimelineStore &s, int sender, int receiver, quint32 tag)
{
    for (const Message &m : s.messages)
        if (m.sender == sender && m.receiver == receiver && m.tag == tag)
            return &m;
    return nullptr;
}

// Forwards everything to a builder and counts the records the reader delivered.
struct CountingSink : TraceSink {
    TraceSink &target;
    int records = 0;
    explicit CountingSink(TraceSink &t) : target(t) {}
    void begin(const TraceMeta &meta) override { target.begin(meta); }
    void addRank(int rank, const QString &node) override { target.addRank(rank, node); }
    void addCall(const MpiCall &c) override { ++records; target.addCall(c); }
    void addSend(const P2PSend &s) override { ++records; target.addSend(s); }
    void addRecv(const P2PRecv &r) override { ++records; target.addRecv(r); }
    void addCollective(const Collective &c) override { ++records; target.addCollective(c); }
    void addUserRegion(const UserRegion &u) override { ++records; target.addUserRegion(u); }
    qint64 stopAfterNs() const override { return target.stopAfterNs(); }
    bool waitingForPartners() const override { return target.waitingForPartners(); }
};

quint16 pyramidValue(const TimelineStore &s, int level, int rank, qint64 ns, Category c)
{
    const Pyramid &p = s.pyramid;
    const int bins = p.binsPerLevel[level];
    const int bin = int(double(ns) * bins / double(p.traceLengthNs));
    return p.data[level][s.rowIndexOfRank(rank)][bin * kCategoryCount + int(c)];
}

struct BuiltWithSummary {
    std::shared_ptr<TimelineStore> store;
    TraceSummary summary;
};

BuiltWithSummary buildWithSummary(const QString &anchor)
{
    BuiltWithSummary b;
    TraceAggregator agg;
    TimelineBuilder builder(agg, 0, kAll, true, 1);
    TraceMeta meta;
    QString error;
    if (!readOtf2(anchor, builder, &meta, &error)) {
        qWarning("%s", qPrintable(error));
        return b;
    }
    b.summary = agg.finish(meta.traceLengthNs);
    b.store = builder.finish();
    return b;
}

QVector<BucketRow> sortedRows(QVector<BucketRow> v)
{
    std::sort(v.begin(), v.end(), [](const BucketRow &a, const BucketRow &b) {
        return std::tie(a.rank, a.commType) < std::tie(b.rank, b.commType);
    });
    return v;
}

QVector<DetailRow> sortedDetails(QVector<DetailRow> v)
{
    std::sort(v.begin(), v.end(), [](const DetailRow &a, const DetailRow &b) {
        return std::tie(a.commType, a.function, a.rank, a.partner, a.collPartners)
             < std::tie(b.commType, b.function, b.rank, b.partner, b.collPartners);
    });
    return v;
}

bool sameAggregate(const QString &what, const QVector<BucketRow> &wantRows, const QVector<DetailRow> &wantDetails,
                   const WindowAggregate &w)
{
    const QVector<BucketRow> want = sortedRows(wantRows);
    const QVector<BucketRow> got = sortedRows(w.rows);
    if (want.size() != got.size()) {
        qWarning("%s: %d rows, want %d", qPrintable(what), int(got.size()), int(want.size()));
        return false;
    }
    for (int i = 0; i < want.size(); ++i) {
        const BucketRow &x = want[i];
        const BucketRow &y = got[i];
        if (x.rank != y.rank || x.commType != y.commType || x.node != y.node || x.sendBytes != y.sendBytes
            || x.recvBytes != y.recvBytes || qAbs(x.lateSender - y.lateSender) > 1e-6
            || qAbs(x.lateReceiver - y.lateReceiver) > 1e-6 || qAbs(x.timeDiff - y.timeDiff) > 1e-6) {
            qWarning("%s row %d (rank %d %s) differs", qPrintable(what), i, x.rank, qPrintable(x.commType));
            return false;
        }
    }
    const QVector<DetailRow> wd = sortedDetails(wantDetails);
    const QVector<DetailRow> gd = sortedDetails(w.details);
    if (wd.size() != gd.size()) {
        qWarning("%s: %d details, want %d", qPrintable(what), int(gd.size()), int(wd.size()));
        return false;
    }
    for (int i = 0; i < wd.size(); ++i) {
        const DetailRow &x = wd[i];
        const DetailRow &y = gd[i];
        if (x.commType != y.commType || x.function != y.function || x.rank != y.rank || x.partner != y.partner
            || x.collPartners != y.collPartners || x.sendBytes != y.sendBytes || x.recvBytes != y.recvBytes) {
            qWarning("%s detail %d (%s rank %d %s) differs", qPrintable(what), i, qPrintable(x.function), x.rank,
                     qPrintable(x.commType));
            return false;
        }
    }
    return true;
}

// Compares the store's aggregate of every whole second with the stage-1 rows/details.
bool aggregateEqualsStage1(const BuiltWithSummary &b)
{
    for (int sec = 0; sec < b.summary.seconds; ++sec) {
        const WindowAggregate w = b.store->aggregate(qint64(sec) * 1'000'000'000, qint64(sec + 1) * 1'000'000'000);
        if (!sameAggregate(QStringLiteral("second %1").arg(sec), b.summary.rows[sec], b.summary.details[sec], w))
            return false;
    }
    return true;
}

// Feeds hand-made records through a TimelineBuilder (declared length 2 s, ranks 0 and 1).
template<typename Feed>
BuiltWithSummary buildSynthetic(Feed feed)
{
    BuiltWithSummary b;
    TraceAggregator agg;
    TimelineBuilder builder(agg, 0, kAll, true, 1);
    builder.addRank(0, QStringLiteral("n"));
    builder.addRank(1, QStringLiteral("n"));
    TraceMeta meta;
    meta.traceLengthNs = 2'000'000'000;
    builder.begin(meta);
    feed(builder);
    b.summary = agg.finish(meta.traceLengthNs);
    b.store = builder.finish();
    return b;
}

} // namespace

class TestTimelineBuilder : public QObject
{
    Q_OBJECT

private slots:
    void categorizeByName()
    {
        QCOMPARE(categorize(QStringLiteral("MPI_Send"), CallKind::P2P), Category::Send);
        QCOMPARE(categorize(QStringLiteral("MPI_Isend"), CallKind::P2P), Category::Send);
        QCOMPARE(categorize(QStringLiteral("MPI_SSEND"), CallKind::P2P), Category::Send);
        QCOMPARE(categorize(QStringLiteral("MPI_Sendrecv"), CallKind::P2P), Category::Recv);
        QCOMPARE(categorize(QStringLiteral("MPI_Recv"), CallKind::P2P), Category::Recv);
        QCOMPARE(categorize(QStringLiteral("MPI_Waitall"), CallKind::P2P), Category::Recv);
        QCOMPARE(categorize(QStringLiteral("MPI_Testany"), CallKind::P2P), Category::Recv);
        QCOMPARE(categorize(QStringLiteral("MPI_Probe"), CallKind::P2P), Category::Recv);
        QCOMPARE(categorize(QStringLiteral("MPI_Allreduce"), CallKind::Collective), Category::Collective);
        QCOMPARE(categorize(QStringLiteral("MPI_Comm_rank"), CallKind::OtherMpi), Category::OtherMpi);
    }

    void ring4Activities()
    {
        QTemporaryDir tmp;
        const Built b = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        QCOMPARE(s.rows.size(), 4);
        QVERIFY(s.complete);
        QCOMPARE(s.traceLengthNs, 2500 * kMs);
        QCOMPARE(row(s, 0).node, QStringLiteral("nodeA"));
        QCOMPARE(row(s, 3).node, QStringLiteral("nodeB"));

        const Activity *send = findActivity(s, 0, QStringLiteral("MPI_Send"), 100 * kMs);
        QVERIFY(send);
        QCOMPARE(send->endNs, 101 * kMs);
        QCOMPARE(send->category, Category::Send);
        const Activity *recv = findActivity(s, 0, QStringLiteral("MPI_Recv"), 101 * kMs);
        QVERIFY(recv);
        QCOMPARE(recv->endNs, 402 * kMs);
        QCOMPARE(recv->category, Category::Recv);
        QCOMPARE(recv->waitNs, qint64(299'000'000));
        QCOMPARE(send->waitNs, qint64(0));

        const Activity *ar = findActivity(s, 2, QStringLiteral("MPI_Allreduce"), 1200 * kMs);
        QVERIFY(ar);
        QCOMPARE(ar->endNs, 1201 * kMs);
        QCOMPARE(ar->category, Category::Collective);
        QCOMPARE(ar->linkCount, quint16(1));
        const CollectiveEntry &ce = s.collectives[linksOf(s, 2, *ar).first()];
        QCOMPARE(ce.rank, 2);
        QCOMPARE(ce.members, (QVector<int>{0, 1, 2, 3}));
        QCOMPARE(ce.bytesSent, quint64(64));
        QCOMPARE(ce.lateSender, 0.0);
        const Activity *ar0 = findActivity(s, 0, QStringLiteral("MPI_Allreduce"), 1000 * kMs);
        QVERIFY(ar0);
        QVERIFY(qAbs(s.collectives[linksOf(s, 0, *ar0).first()].lateSender - 0.200) < 1e-9);

        for (const RankRow &r : s.rows) {
            for (int i = 0; i < r.activities.size(); ++i) {
                QVERIFY(s.functions.value(r.activities[i].function) != QLatin1String("main"));
                if (i > 0)
                    QVERIFY(r.activities[i - 1].startNs <= r.activities[i].startNs);
            }
        }
        QCOMPARE(row(s, 0).activities.size(), 3);
        QCOMPARE(row(s, 2).activities.size(), 5);
    }

    void ring4Messages()
    {
        QTemporaryDir tmp;
        const Built b = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        QCOMPARE(s.messages.size(), 7);
        int matched = 0;
        for (const Message &m : s.messages)
            matched += m.receiver >= 0 ? 1 : 0;
        QCOMPARE(matched, 6);
        QCOMPARE(s.unmatchedMessages, 1);

        const Message *lost = findMessage(s, 3, -1, 11);
        QVERIFY(lost);
        QCOMPARE(lost->recvNs, qint64(-1));
        QCOMPARE(lost->sendNs, 2400 * kMs);
        QCOMPARE(lost->bytes, quint64(100));

        const Message *m30 = findMessage(s, 3, 0, 7);
        QVERIFY(m30);
        QCOMPARE(m30->sendNs, 400 * kMs);
        QCOMPARE(m30->recvNs, 402 * kMs);
        QCOMPARE(m30->bytes, quint64(1000));
        QVERIFY(qAbs(m30->lateSender - 0.299) < 1e-9);

        const Message *sub = findMessage(s, 3, 2, 5);
        QVERIFY(sub);
        QCOMPARE(sub->bytes, quint64(200));
        const Message *ssend = findMessage(s, 1, 2, 9);
        QVERIFY(ssend);
        QVERIFY(qAbs(ssend->lateReceiver - 0.300) < 1e-9);

        // Messages sorted by sendNs; rows list their messages sorted by sendNs.
        for (int i = 1; i < s.messages.size(); ++i)
            QVERIFY(s.messages[i - 1].sendNs <= s.messages[i].sendNs);
        const RankRow &r2 = row(s, 2);
        QVector<const Message *> received2;
        for (int i = 0; i < r2.messages.size(); ++i) {
            const Message &m = s.messages[r2.messages[i]];
            QVERIFY(m.sender == 2 || m.receiver == 2);
            if (i > 0)
                QVERIFY(s.messages[r2.messages[i - 1]].sendNs <= m.sendNs);
            if (m.receiver == 2 && m.recvNs >= 2000 * kMs)
                received2.append(&m);
        }
        QCOMPARE(r2.messages.size(), 4);
        QCOMPARE(received2.size(), 2);
        QVERIFY(received2.contains(sub));
        QVERIFY(received2.contains(ssend));
        // The unmatched send is listed on the sender only.
        QVERIFY(row(s, 3).messages.contains(int(lost - s.messages.constData())));
        QVERIFY(!row(s, 0).messages.contains(int(lost - s.messages.constData())));

        // Links: R0's MPI_Recv completes the message from R3, R3's MPI_Send started it.
        const Activity *recv = findActivity(s, 0, QStringLiteral("MPI_Recv"), 101 * kMs);
        QVERIFY(recv);
        QCOMPARE(linksOf(s, 0, *recv), QVector<int>{int(m30 - s.messages.constData())});
        const Activity *send = findActivity(s, 3, QStringLiteral("MPI_Send"), 400 * kMs);
        QVERIFY(send);
        QCOMPARE(linksOf(s, 3, *send), QVector<int>{int(m30 - s.messages.constData())});
        const Activity *lostSend = findActivity(s, 3, QStringLiteral("MPI_Send"), 2400 * kMs);
        QVERIFY(lostSend);
        QCOMPARE(linksOf(s, 3, *lostSend), QVector<int>{int(lost - s.messages.constData())});
    }

    void ring4Pyramid()
    {
        QTemporaryDir tmp;
        const Built b = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        const Pyramid &p = s.pyramid;
        QCOMPARE(p.levels, 4);
        QCOMPARE(p.binsPerLevel, (QVector<int>{1024, 4096, 16384, 65536}));
        QCOMPARE(p.traceLengthNs, 2500 * kMs);
        QCOMPARE(p.data.size(), 4);
        QCOMPARE(p.data[3].size(), 4);
        QCOMPARE(p.data[3][0].size(), 65536 * kCategoryCount);

        QVERIFY(qAbs(int(pyramidValue(s, 0, 0, 250 * kMs, Category::Wait)) - 65535) <= 655);
        QVERIFY(pyramidValue(s, 0, 0, 250 * kMs, Category::Recv) <= 655);
        QVERIFY(qAbs(int(pyramidValue(s, 0, 1, 250 * kMs, Category::Compute)) - 65535) <= 655);
        // Bin containing 401 ms on R0: after the wait (ends at 400 ms), still in the receive.
        QVERIFY(qAbs(int(pyramidValue(s, 3, 0, 401 * kMs, Category::Recv)) - 65535) <= 655);
        // R2's MPI_Allreduce: collective at the finest level.
        QVERIFY(qAbs(int(pyramidValue(s, 3, 2, 1200 * kMs + 500'000, Category::Collective)) - 65535) <= 655);
    }

    void pyramidWithoutActivities()
    {
        QTemporaryDir tmp;
        const Built b = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))), 0, kAll, false, 2);
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        QVERIFY(!s.complete);
        QCOMPARE(s.pyramid.levels, 2);
        for (const RankRow &r : s.rows) {
            QVERIFY(r.activities.isEmpty());
            QVERIFY(r.messages.isEmpty());
        }
        QVERIFY(s.messages.isEmpty());
        QVERIFY(qAbs(int(pyramidValue(s, 0, 0, 250 * kMs, Category::Wait)) - 65535) <= 655);
        QVERIFY(qAbs(int(pyramidValue(s, 1, 0, 250 * kMs, Category::Wait)) - 65535) <= 655);
    }

    void nonblockingWaitPatched()
    {
        QTemporaryDir tmp;
        const Built b = build(writeNonblockingOtf2(tmp.filePath(QStringLiteral("nb"))));
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        const Activity *wait = findActivity(s, 0, QStringLiteral("MPI_Wait"), 110 * kMs);
        QVERIFY(wait);
        QCOMPARE(wait->endNs, 250 * kMs);
        QCOMPARE(wait->category, Category::Recv);
        QCOMPARE(wait->waitNs, qint64(90'000'000));
        const QVector<int> l = linksOf(s, 0, *wait);
        QCOMPARE(l.size(), 1);
        const Message &m = s.messages[l.first()];
        QCOMPARE(m.sender, 1);
        QCOMPARE(m.receiver, 0);
        QCOMPARE(m.bytes, quint64(400));
        QCOMPARE(m.sendNs, 200 * kMs);
        QCOMPARE(m.recvNs, 250 * kMs);
        const Activity *isend = findActivity(s, 1, QStringLiteral("MPI_Isend"), 200 * kMs);
        QVERIFY(isend);
        QCOMPARE(linksOf(s, 1, *isend), l);
    }

    void windowKeepsOnlyOverlap()
    {
        QTemporaryDir tmp;
        const Built b = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))), 1000 * kMs, 1300 * kMs);
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        QVERIFY(!s.complete);
        QCOMPARE(s.fromNs, 1000 * kMs);
        QCOMPARE(s.toNs, 1300 * kMs);
        QCOMPARE(s.pyramid.levels, 0);
        QVERIFY(s.messages.isEmpty());
        int count = 0;
        for (const RankRow &r : s.rows) {
            QVERIFY(r.messages.isEmpty());
            for (const Activity &a : r.activities) {
                QCOMPARE(s.functions.value(a.function), QStringLiteral("MPI_Allreduce"));
                QCOMPARE(a.linkCount, quint16(1));
                ++count;
            }
        }
        QCOMPARE(count, 4);
        QCOMPARE(s.collectives.size(), 4);
        // main [0, 2500] overlaps the window.
        QCOMPARE(row(s, 0).userSpans.size(), 1);
    }

    void waitallLinksAllReceives()
    {
        QTemporaryDir tmp;
        const Built b = build(writeWaitallOtf2(tmp.filePath(QStringLiteral("wa"))));
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        const Activity *wa = findActivity(s, 1, QStringLiteral("MPI_Waitall"), 150 * kMs);
        QVERIFY(wa);
        QCOMPARE(wa->linkCount, quint16(2));
        QSet<quint32> tags;
        for (int i : linksOf(s, 1, *wa)) {
            QCOMPARE(s.messages[i].receiver, 1);
            QCOMPARE(s.messages[i].recvNs, 300 * kMs);
            tags.insert(s.messages[i].tag);
        }
        QCOMPARE(tags, (QSet<quint32>{2, 3}));
        QCOMPARE(wa->waitNs, qint64(100'000'000));
        QVERIFY(wa->waitNs <= wa->endNs - wa->startNs);
        // R0's MPI_Waitall only completes sends: no wait, no links.
        const Activity *wa0 = findActivity(s, 0, QStringLiteral("MPI_Waitall"), 260 * kMs);
        QVERIFY(wa0);
        QCOMPARE(wa0->waitNs, qint64(0));
        QCOMPARE(wa0->linkCount, quint16(0));
    }

    void waitIsCappedAtDuration()
    {
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, true, 1);
        builder.addRank(0, QStringLiteral("n"));
        builder.addRank(1, QStringLiteral("n"));
        TraceMeta meta;
        meta.traceLengthNs = 1000;
        builder.begin(meta);
        // R1's MPI_Recv [100, 150] completes a message whose send entered at 400 (clock skew).
        builder.addCall({1, CallKind::P2P, QStringLiteral("MPI_Recv"), 100, 150});
        builder.addRecv({1, 0, 0, 1, 8, QStringLiteral("MPI_Recv"), 100, 150});
        builder.addCall({0, CallKind::P2P, QStringLiteral("MPI_Send"), 400, 401});
        builder.addSend({0, 1, 0, 1, 8, QStringLiteral("MPI_Send"), 400, 401});
        agg.finish(1000);
        const auto s = builder.finish();
        const Activity *recv = findActivity(*s, 1, QStringLiteral("MPI_Recv"), 100);
        QVERIFY(recv);
        QCOMPARE(recv->waitNs, qint64(50));
        QCOMPARE(recv->linkCount, quint16(1));
    }

    void userSpansFromPhases()
    {
        QTemporaryDir tmp;
        const Built b = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))));
        QVERIFY(b.store);
        const TimelineStore &s = *b.store;
        const RankRow &r0 = row(s, 0);
        QCOMPARE(r0.maxDepth, quint8(2));
        QStringList names;
        for (int i = 0; i < r0.userSpans.size(); ++i) {
            const UserSpan &u = r0.userSpans[i];
            names << QStringLiteral("%1:%2").arg(s.functions.value(u.function)).arg(u.depth);
            if (i > 0) {
                const UserSpan &p = r0.userSpans[i - 1];
                QVERIFY(p.startNs < u.startNs || (p.startNs == u.startNs && p.depth <= u.depth));
            }
        }
        QCOMPARE(names, (QStringList{QStringLiteral("main:0"), QStringLiteral("phase_a:1"),
                                     QStringLiteral("send_to:2"), QStringLiteral("phase_b:1")}));
        QVERIFY(!s.functions.contains(QStringLiteral("TRACE BUFFER FLUSH")));
        QCOMPARE(r0.userSpans[2].startNs, 500 * kMs);
        QCOMPARE(r0.userSpans[2].endNs, 600 * kMs);
    }

    void overviewKeepsOnlyLongUserSpans()
    {
        // Pyramid-only mode: spans shorter than a level-1 bin (10 s / 4096 = 2.44 ms) are dropped.
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, false, 2);
        builder.addRank(0, QStringLiteral("n"));
        TraceMeta meta;
        meta.traceLengthNs = 10'000 * kMs;
        builder.begin(meta);
        builder.addUserRegion({0, QStringLiteral("tiny"), 1, 1000 * kMs, 1000 * kMs + 100});
        builder.addUserRegion({0, QStringLiteral("long"), 1, 2000 * kMs, 2010 * kMs});
        builder.addUserRegion({0, QStringLiteral("main"), 0, 0, 10'000 * kMs});
        agg.finish(meta.traceLengthNs);
        const auto s = builder.finish();
        const RankRow &r = row(*s, 0);
        QStringList names;
        for (const UserSpan &u : r.userSpans)
            names << s->functions.value(u.function);
        QCOMPARE(names, (QStringList{QStringLiteral("main"), QStringLiteral("long")}));
        QCOMPARE(r.maxDepth, quint8(1));
        QVERIFY(r.activities.isEmpty());
    }

    void overviewPhasesKeepsPhaseSpans()
    {
        QTemporaryDir tmp;
        const Built b = build(writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))), 0, kAll, false, 2);
        QVERIFY(b.store);
        QStringList names;
        for (const UserSpan &u : row(*b.store, 0).userSpans)
            names << b.store->functions.value(u.function);
        // All phases spans (>= 100 ms) exceed the 0.73 ms level-1 bin of the 3 s trace.
        QCOMPARE(names, (QStringList{QStringLiteral("main"), QStringLiteral("phase_a"),
                                     QStringLiteral("send_to"), QStringLiteral("phase_b")}));
    }

    void overviewUserSpansAreBounded()
    {
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, false, 2);
        builder.addRank(0, QStringLiteral("n"));
        TraceMeta meta;
        meta.traceLengthNs = 10'000 * kMs;
        builder.begin(meta);
        for (qint64 i = 0; i < 100'000; ++i) // 1 µs regions every 100 µs
            builder.addUserRegion({0, QStringLiteral("f"), 1, i * 100'000, i * 100'000 + 1000});
        agg.finish(meta.traceLengthNs);
        const auto s = builder.finish();
        QVector<int> perDepth(kMaxUserDepth + 1, 0);
        for (const UserSpan &u : row(*s, 0).userSpans)
            ++perDepth[u.depth];
        for (int n : perDepth)
            QVERIFY(n <= 4096);
    }

    void functionTableIsBounded()
    {
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, true, 0);
        builder.addRank(0, QStringLiteral("n"));
        TraceMeta meta;
        meta.traceLengthNs = 100'000'000;
        builder.begin(meta);
        for (int i = 0; i < 70'000; ++i)
            builder.addUserRegion({0, QStringLiteral("fn_%1").arg(i), 0, qint64(i) * 1000, qint64(i) * 1000 + 500});
        builder.addCall({0, CallKind::P2P, QStringLiteral("MPI_Send"), 99'000'000, 99'000'001});
        agg.finish(meta.traceLengthNs);
        const auto s = builder.finish();
        QVERIFY(s->functions.size() <= 65536);
        QCOMPARE(s->functions.last(), QStringLiteral("(other)"));
        const RankRow &r = row(*s, 0);
        QCOMPARE(r.userSpans.size(), 70'000);
        for (const UserSpan &u : r.userSpans)
            QVERIFY(u.function < s->functions.size());
        for (const Activity &a : r.activities)
            QVERIFY(a.function < s->functions.size());
        QCOMPARE(s->functions.value(r.userSpans.first().function), QStringLiteral("fn_0"));
        QCOMPARE(s->functions.value(r.userSpans.last().function), QStringLiteral("(other)"));
    }

    void deepNestingIsBounded()
    {
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, true, 2);
        builder.addRank(0, QStringLiteral("n"));
        TraceMeta meta;
        meta.traceLengthNs = 1000;
        builder.begin(meta);
        for (int d = 39; d >= 0; --d) // innermost leaves first
            builder.addUserRegion({0, QStringLiteral("f%1").arg(d), d, qint64(d), qint64(1000 - d)});
        agg.finish(1000);
        const auto s = builder.finish();
        const RankRow &r = row(*s, 0);
        QCOMPARE(r.maxDepth, quint8(15));
        QCOMPARE(r.userSpans.size(), 40);
        int atCap = 0;
        for (const UserSpan &u : r.userSpans) {
            QVERIFY(u.depth <= 15);
            atCap += u.depth == 15 ? 1 : 0;
        }
        QCOMPARE(atCap, 25);
        QCOMPARE(r.userSpans.first().depth, quint8(0));
    }

    void aggregateMatchesSeconds()
    {
        QTemporaryDir tmp;
        const BuiltWithSummary b = buildWithSummary(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(b.store);
        QCOMPARE(b.summary.seconds, 3);
        QVERIFY(aggregateEqualsStage1(b));
    }

    void aggregateMatchesSecondsWaitall()
    {
        QTemporaryDir tmp;
        const BuiltWithSummary b = buildWithSummary(writeWaitallOtf2(tmp.filePath(QStringLiteral("wa"))));
        QVERIFY(b.store);
        QVERIFY(aggregateEqualsStage1(b));
        // Per-message rule: R1's Waitall [150, 300] completes two receives, both late.
        double late = 0;
        for (const BucketRow &r : b.store->aggregate(0, 1'000'000'000).rows)
            if (r.rank == 1)
                late += r.lateSender;
        QVERIFY(late > 0.1 + 1e-9);
    }

    void aggregateMatchesSecondsNonblocking()
    {
        QTemporaryDir tmp;
        const BuiltWithSummary b = buildWithSummary(writeNonblockingOtf2(tmp.filePath(QStringLiteral("nb"))));
        QVERIFY(b.store);
        QVERIFY(aggregateEqualsStage1(b));
    }

    void aggregateSubSecond()
    {
        QTemporaryDir tmp;
        const BuiltWithSummary b = buildWithSummary(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(b.store);
        const QVector<BucketRow> rows = sortedRows(b.store->aggregate(0, 300 * kMs).rows);
        const BucketRow *r0 = nullptr;
        const BucketRow *r3 = nullptr;
        for (const BucketRow &r : rows) {
            if (r.rank == 0 && r.commType == QLatin1String("p2p"))
                r0 = &r;
            if (r.rank == 3 && r.commType == QLatin1String("p2p"))
                r3 = &r;
        }
        QVERIFY(r0 && r3);
        QCOMPARE(r0->sendBytes, qint64(1000));
        QCOMPARE(r0->recvBytes, qint64(1000));
        QVERIFY(qAbs(r0->lateSender - 0.299) < 1e-6);
        QCOMPARE(r3->sendBytes, qint64(0));
        QCOMPARE(r3->recvBytes, qint64(1000));
    }

    void aggregateOutsideLoadedWindowIsEmpty()
    {
        QTemporaryDir tmp;
        const Built b = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))), 1000 * kMs, 1300 * kMs);
        QVERIFY(b.store);
        QVERIFY(b.store->aggregate(0, 500 * kMs).rows.isEmpty());
        QVERIFY(!b.store->aggregate(1000 * kMs, 1300 * kMs).rows.isEmpty());
    }

    void aggregateUnmatchedReceive()
    {
        const BuiltWithSummary b = buildSynthetic([](TimelineBuilder &t) {
            t.addCall({0, CallKind::P2P, QStringLiteral("MPI_Send"), 10 * kMs, 20 * kMs});
            t.addSend({0, 1, 1, 3, 400, QStringLiteral("MPI_Send"), 10 * kMs, 20 * kMs});
            t.addCall({1, CallKind::P2P, QStringLiteral("MPI_Recv"), 1500 * kMs, 1600 * kMs});
            t.addRecv({1, 0, 1, 4, 300, QStringLiteral("MPI_Recv"), 1500 * kMs, 1600 * kMs});
        });
        QVERIFY(b.store);
        QCOMPARE(b.store->unmatchedMessages, 2);
        bool found = false;
        for (const Message &m : b.store->messages)
            found = found || (m.sendNs < 0 && m.receiver == 1 && m.sender == 0 && m.bytes == 300);
        QVERIFY(found);
        QVERIFY(aggregateEqualsStage1(b));
    }

    void aggregateSelfSendrecv()
    {
        const BuiltWithSummary b = buildSynthetic([](TimelineBuilder &t) {
            t.addCall({0, CallKind::P2P, QStringLiteral("MPI_Sendrecv"), 10 * kMs, 20 * kMs});
            t.addSend({0, 0, 1, 5, 100, QStringLiteral("MPI_Sendrecv"), 10 * kMs, 20 * kMs});
            t.addRecv({0, 0, 1, 5, 100, QStringLiteral("MPI_Sendrecv"), 10 * kMs, 20 * kMs});
        });
        QVERIFY(b.store);
        QVERIFY(aggregateEqualsStage1(b));
        const WindowAggregate w = b.store->aggregate(0, 1'000'000'000);
        QCOMPARE(w.rows.size(), 1);
        QCOMPARE(w.rows[0].sendBytes, qint64(100));
        QCOMPARE(w.rows[0].recvBytes, qint64(100));
    }

    void windowedAggregateEqualsCompleteStore()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.filePath(QStringLiteral("r4")));
        const Built full = build(anchor);
        QVERIFY(full.store);
        // R0's MPI_Recv [101, 402] enters inside [0, 300 ms], its send (R3) enters at 400 ms.
        const qint64 windows[][2] = {{0, 300 * kMs}, {0, 120 * kMs}, {100 * kMs, 300 * kMs}, {50 * kMs, 450 * kMs}};
        for (const auto &w : windows) {
            const Built part = build(anchor, w[0], w[1]);
            QVERIFY(part.store);
            const WindowAggregate want = full.store->aggregate(w[0], w[1]);
            QVERIFY(!want.rows.isEmpty());
            QVERIFY2(sameAggregate(QStringLiteral("window %1-%2").arg(w[0]).arg(w[1]), want.rows, want.details,
                                   part.store->aggregate(w[0], w[1])),
                     "windowed store differs from complete store");
        }
    }

    // With a byte cap (detail windows), the builder stops keeping once the appended records
    // would exceed it, reports overBudget() and trips the cancel flag so the read stops early.
    void byteCapStopsBuildAndCancelsRead()
    {
        QTemporaryDir tmp;
        const QString anchor = writeManyMessagesOtf2(tmp.filePath(QStringLiteral("many")), 4, 5000);
        QVERIFY(!anchor.isEmpty());
        const quint64 cap = 64 * 1024;
        {
            TraceAggregator agg;
            TimelineBuilder builder(agg, 0, kAll, true, 0);
            std::atomic_bool cancel{false};
            builder.setByteCap(cap, &cancel);
            TraceMeta meta;
            QString error;
            QVERIFY(!readOtf2(anchor, builder, &meta, &error, &cancel)); // stopped early
            QVERIFY(cancel.load());
            QVERIFY(builder.overBudget());
            QVERIFY(builder.keptBytes() <= cap);
            const auto store = builder.finish();
            QVERIFY(store);
            QVERIFY(!store->complete);
        }
        {
            // Without a cap (or below it) nothing changes.
            TraceAggregator agg;
            TimelineBuilder builder(agg, 0, kAll, true, 0);
            std::atomic_bool cancel{false};
            builder.setByteCap(256 * 1024 * 1024, &cancel);
            TraceMeta meta;
            QString error;
            QVERIFY(readOtf2(anchor, builder, &meta, &error, &cancel));
            QVERIFY(!cancel.load());
            QVERIFY(!builder.overBudget());
            QVERIFY(builder.keptBytes() > cap);
            agg.finish(meta.traceLengthNs);
            QVERIFY(builder.finish()->complete);
        }
    }

    // A window build stops reading once the window is over and nothing in it waits for a
    // partner any more: same store as a build that reads to the end (user spans still open at
    // the stop may end later than recorded, but not inside the window), fewer records read.
    void windowBuildStopsAfterWindow()
    {
        QTemporaryDir tmp;
        struct Case {
            QString anchor;
            qint64 from;
            qint64 to;
            bool expectEarly;
        };
        const QString many = writeManyMessagesOtf2(tmp.filePath(QStringLiteral("many")), 4, 3000);
        const QString r4 = writeRing4Otf2(tmp.filePath(QStringLiteral("r4")));
        const QString wa = writeWaitallOtf2(tmp.filePath(QStringLiteral("wa")));
        const QString nb = writeNonblockingOtf2(tmp.filePath(QStringLiteral("nb")));
        const QVector<Case> cases{
            {many, 10 * kMs, 12 * kMs, true},
            {many, 50 * kMs, 80 * kMs, true}, // trace ~301 ms long: stops at about a third
            {many, 0, 1 * kMs, true},
            // R0's MPI_Recv [101, 402] enters inside the window, its send (R3) enters at 400 ms.
            {r4, 0, 300 * kMs, false},
            {r4, 100 * kMs, 300 * kMs, false},
            {wa, 0, 50 * kMs, false},
            {nb, 0, 50 * kMs, false},
        };
        for (const Case &c : cases) {
            const QString label = QStringLiteral("%1 [%2, %3]").arg(c.anchor).arg(c.from).arg(c.to);
            int recordsAll = 0;
            int recordsEarly = 0;
            const auto run = [&](bool early, int *records) {
                TraceAggregator agg;
                TimelineBuilder builder(agg, c.from, c.to, true, 0);
                builder.setEarlyStop(early);
                CountingSink counter(builder);
                TraceMeta meta;
                QString error;
                const bool ok = readOtf2(c.anchor, counter, &meta, &error);
                if (!ok)
                    qWarning("%s: %s", qPrintable(label), qPrintable(error));
                agg.finish(meta.traceLengthNs);
                *records = counter.records;
                return ok ? builder.finish() : nullptr;
            };
            const auto all = run(false, &recordsAll);
            const auto early = run(true, &recordsEarly);
            QVERIFY2(all && early, qPrintable(label));
            QVERIFY2(recordsEarly <= recordsAll, qPrintable(label));
            if (c.expectEarly)
                QVERIFY2(recordsEarly < recordsAll / 2,
                         qPrintable(QStringLiteral("%1: %2 of %3 records").arg(label).arg(recordsEarly).arg(recordsAll)));
            QCOMPARE(early->traceLengthNs, all->traceLengthNs);
            QCOMPARE(early->fromNs, all->fromNs);
            QCOMPARE(early->toNs, all->toNs);
            QCOMPARE(early->rows.size(), all->rows.size());
            for (int r = 0; r < all->rows.size(); ++r) {
                const RankRow &a = all->rows[r];
                const RankRow &e = early->rows[r];
                QCOMPARE(e.activities.size(), a.activities.size());
                for (int i = 0; i < a.activities.size(); ++i) {
                    QCOMPARE(e.activities[i].startNs, a.activities[i].startNs);
                    QCOMPARE(e.activities[i].endNs, a.activities[i].endNs);
                    QCOMPARE(e.activities[i].waitNs, a.activities[i].waitNs);
                    QCOMPARE(e.activities[i].linkCount, a.activities[i].linkCount);
                    QCOMPARE(early->functions.value(e.activities[i].function), all->functions.value(a.activities[i].function));
                }
                QCOMPARE(e.userSpans.size(), a.userSpans.size());
                for (int i = 0; i < a.userSpans.size(); ++i) {
                    QCOMPARE(e.userSpans[i].startNs, a.userSpans[i].startNs);
                    QCOMPARE(e.userSpans[i].depth, a.userSpans[i].depth);
                    if (e.userSpans[i].endNs != a.userSpans[i].endNs) {
                        QVERIFY2(e.userSpans[i].endNs >= c.to && a.userSpans[i].endNs >= c.to, qPrintable(label));
                    }
                }
            }
            QCOMPARE(early->messages.size(), all->messages.size());
            for (int i = 0; i < all->messages.size(); ++i) {
                const Message &a = all->messages[i];
                const Message &e = early->messages[i];
                QCOMPARE(e.sender, a.sender);
                QCOMPARE(e.receiver, a.receiver);
                QCOMPARE(e.sendNs, a.sendNs);
                QCOMPARE(e.recvNs, a.recvNs);
                QCOMPARE(e.lateSender, a.lateSender);
                QCOMPARE(e.lateReceiver, a.lateReceiver);
            }
            QCOMPARE(early->collectives.size(), all->collectives.size());
            for (int i = 0; i < all->collectives.size(); ++i)
                QCOMPARE(early->collectives[i].lateSender, all->collectives[i].lateSender);
        }
    }

    // Downgrade cap: a full build that crosses the cap drops everything kept so far and
    // continues as a pyramid-only overview; the result equals an overview built as such from
    // the start (pyramid, long user spans, unmatched count) and the read is not cancelled.
    void downgradeCapSwitchesToOverview()
    {
        QTemporaryDir tmp;
        const QStringList anchors{writeWaitallOtf2(tmp.filePath(QStringLiteral("wa"))),
                                  writeNonblockingOtf2(tmp.filePath(QStringLiteral("nb"))),
                                  writePhasesOtf2(tmp.filePath(QStringLiteral("ph"))),
                                  writeManyMessagesOtf2(tmp.filePath(QStringLiteral("many")), 4, 3000)};
        for (const QString &a : anchors) {
            const Built full = build(a);
            QVERIFY(full.store && full.store->complete);
            const Built overview = build(a, 0, kAll, false, 4);
            QVERIFY(overview.store);

            TraceAggregator agg;
            TimelineBuilder builder(agg, 0, kAll, true, 4);
            std::atomic_bool cancel{false};
            const quint64 cap = std::max<quint64>(64, (full.store->bytesUsed() - pyramidBytes(int(full.store->rows.size()), 4)) / 3);
            builder.setDowngradeCap(cap);
            TraceMeta meta;
            QString error;
            QVERIFY2(readOtf2(a, builder, &meta, &error, &cancel), qPrintable(error));
            QVERIFY(!cancel.load());
            QVERIFY2(builder.downgraded(), qPrintable(a));
            QVERIFY(!builder.overBudget());
            QVERIFY(builder.keptBytesAtDowngrade() > 0);
            QVERIFY(builder.keptBytesAtDowngrade() <= cap);
            agg.finish(meta.traceLengthNs);
            const auto store = builder.finish();
            QVERIFY(store);
            QVERIFY(!store->complete);
            QVERIFY(store->messages.isEmpty());
            QVERIFY(store->collectives.isEmpty());
            QCOMPARE(store->unmatchedMessages, overview.store->unmatchedMessages);
            QCOMPARE(store->rows.size(), overview.store->rows.size());
            for (int r = 0; r < store->rows.size(); ++r) {
                const RankRow &got = store->rows[r];
                const RankRow &want = overview.store->rows[r];
                QVERIFY(got.activities.isEmpty());
                QVERIFY(got.links.isEmpty());
                QCOMPARE(got.userSpans.size(), want.userSpans.size());
                for (int i = 0; i < got.userSpans.size(); ++i) {
                    QCOMPARE(got.userSpans[i].startNs, want.userSpans[i].startNs);
                    QCOMPARE(got.userSpans[i].endNs, want.userSpans[i].endNs);
                    QCOMPARE(store->functions.value(got.userSpans[i].function),
                             overview.store->functions.value(want.userSpans[i].function));
                }
            }
            QCOMPARE(store->pyramid.levels, overview.store->pyramid.levels);
            QVERIFY2(store->pyramid.data == overview.store->pyramid.data, qPrintable(a));
        }
    }

    // Below the downgrade cap the build stays complete.
    void downgradeCapNotReachedKeepsFullStore()
    {
        QTemporaryDir tmp;
        const QString anchor = writeWaitallOtf2(tmp.path());
        TraceAggregator agg;
        TimelineBuilder builder(agg, 0, kAll, true, 4);
        builder.setDowngradeCap(256 * 1024 * 1024);
        TraceMeta meta;
        QString error;
        QVERIFY(readOtf2(anchor, builder, &meta, &error));
        QVERIFY(!builder.downgraded());
        QCOMPARE(builder.keptBytesAtDowngrade(), quint64(0));
        agg.finish(meta.traceLengthNs);
        QVERIFY(builder.finish()->complete);
    }

    void estimateIsUpperBound()
    {
        QTemporaryDir tmp;
        const QStringList anchors{writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))),
                                  writeNonblockingOtf2(tmp.filePath(QStringLiteral("nb"))),
                                  writeWaitallOtf2(tmp.filePath(QStringLiteral("wa"))),
                                  writePhasesOtf2(tmp.filePath(QStringLiteral("ph")))};
        for (const QString &a : anchors) {
            const Built b = build(a);
            QVERIFY(b.store);
            QVERIFY(b.store->bytesUsed() >= pyramidBytes(int(b.store->rows.size()), 4));
            const quint64 est = estimateTimelineBytes(b.meta.eventCount, b.meta.hasMetrics, 4, 4);
            QVERIFY2(est >= b.store->bytesUsed(),
                     qPrintable(QStringLiteral("%1: estimate %2 < used %3 (events %4)")
                                    .arg(a).arg(est).arg(b.store->bytesUsed()).arg(b.meta.eventCount)));
        }
        QCOMPARE(pyramidBytes(1, 1), quint64(1024 * 6 * 2));
        QCOMPARE(pyramidBytes(2, 2), quint64(2 * (1024 + 4096) * 6 * 2));
    }
};

QTEST_GUILESS_MAIN(TestTimelineBuilder)
#include "tst_trace_timeline_builder.moc"
