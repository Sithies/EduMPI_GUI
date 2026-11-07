#include "ring4_scenario.h"

#include <QtTest>

using namespace trace;

namespace {

constexpr quint64 kWorld = 1;
constexpr quint64 kSub = 2;
constexpr qint64 kMs = 1'000'000;

void call(TraceAggregator &a, int rank, const QString &fn, qint64 enterMs, qint64 leaveMs)
{
    a.addCall({rank, classifyMpiFunction(fn), fn, enterMs * kMs, leaveMs * kMs});
}

void send(TraceAggregator &a, int rank, int peer, quint64 comm, quint32 tag, quint64 bytes,
          const QString &fn, qint64 enterMs, qint64 leaveMs)
{
    call(a, rank, fn, enterMs, leaveMs);
    a.addSend({rank, peer, comm, tag, bytes, fn, enterMs * kMs, leaveMs * kMs});
}

void recv(TraceAggregator &a, int rank, int peer, quint64 comm, quint32 tag, quint64 bytes,
          const QString &fn, qint64 enterMs, qint64 leaveMs)
{
    call(a, rank, fn, enterMs, leaveMs);
    a.addRecv({rank, peer, comm, tag, bytes, fn, enterMs * kMs, leaveMs * kMs});
}

void allreduce(TraceAggregator &a, int rank, qint64 enterMs, qint64 leaveMs)
{
    const QString fn = QStringLiteral("MPI_Allreduce");
    call(a, rank, fn, enterMs, leaveMs);
    a.addCollective({rank, kWorld, {0, 1, 2, 3}, fn, enterMs * kMs, leaveMs * kMs, 64, 64});
}

const BucketRow *findRow(const QVector<BucketRow> &rows, int rank, const QString &type)
{
    for (const BucketRow &r : rows)
        if (r.rank == rank && r.commType == type)
            return &r;
    return nullptr;
}

const DetailRow *findDetail(const QVector<DetailRow> &rows, const QString &fn, int rank, int partner)
{
    for (const DetailRow &r : rows)
        if (r.function == fn && r.rank == rank && r.partner == partner)
            return &r;
    return nullptr;
}

bool fuzzy(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

void checkRow(const QVector<BucketRow> &rows, int rank, const QString &type, const QString &node,
              qint64 sendB, qint64 recvB, double ls, double lr, double td)
{
    const BucketRow *r = findRow(rows, rank, type);
    QVERIFY2(r, qPrintable(QStringLiteral("missing row rank %1 %2").arg(rank).arg(type)));
    QCOMPARE(r->node, node);
    QCOMPARE(r->sendBytes, sendB);
    QCOMPARE(r->recvBytes, recvB);
    QVERIFY2(fuzzy(r->lateSender, ls), qPrintable(QString::number(r->lateSender)));
    QVERIFY2(fuzzy(r->lateReceiver, lr), qPrintable(QString::number(r->lateReceiver)));
    QVERIFY2(fuzzy(r->timeDiff, td), qPrintable(QString::number(r->timeDiff)));
}

void checkP2PDetail(const QVector<DetailRow> &rows, const QString &fn, int rank, int partner,
                    qint64 sendB, qint64 recvB)
{
    const DetailRow *d = findDetail(rows, fn, rank, partner);
    QVERIFY2(d, qPrintable(QStringLiteral("missing detail %1 %2->%3").arg(fn).arg(rank).arg(partner)));
    QCOMPARE(d->commType, QStringLiteral("p2p"));
    QVERIFY(d->collPartners.isEmpty());
    QCOMPARE(d->sendBytes, sendB);
    QCOMPARE(d->recvBytes, recvB);
}

using Matrix = QVector<QVector<qint64>>;

// Mirrors Database_Thread::detailed_p2p_Query's matrix building from detail rows.
void buildMatrices(const QVector<DetailRow> &rows, Matrix &p2pSend, Matrix &p2pRecv,
                   Matrix &collSend, Matrix &collRecv)
{
    const Matrix zero(4, QVector<qint64>(4, 0));
    p2pSend = p2pRecv = collSend = collRecv = zero;
    for (const DetailRow &d : rows) {
        if (d.commType == QLatin1String("p2p")) {
            p2pSend[d.rank][d.partner] += d.sendBytes;
            p2pRecv[d.rank][d.partner] += d.recvBytes;
        } else {
            QVector<int> partners;
            for (int byte = 0; byte < d.collPartners.size(); ++byte)
                for (int bit = 0; bit < 8; ++bit)
                    if (uchar(d.collPartners[byte]) & (1u << bit))
                        partners.append(byte * 8 + bit);
            if (partners.isEmpty())
                continue;
            const qint64 share = d.sendBytes / partners.size();
            for (int p : partners) {
                collSend[d.rank][p] += share;
                collRecv[p][d.rank] += share;
            }
        }
    }
}

} // namespace

void feedRing4(TraceAggregator &a)
{
    a.addRank(0, QStringLiteral("nodeA"));
    a.addRank(1, QStringLiteral("nodeA"));
    a.addRank(2, QStringLiteral("nodeB"));
    a.addRank(3, QStringLiteral("nodeB"));

    const QString mSend = QStringLiteral("MPI_Send");
    const QString mRecv = QStringLiteral("MPI_Recv");

    // Second 0: ring 0 -> 1 -> 2 -> 3 -> 0, tag 7, 1000 B
    send(a, 0, 1, kWorld, 7, 1000, mSend, 100, 101);
    recv(a, 0, 3, kWorld, 7, 1000, mRecv, 101, 402);
    send(a, 1, 2, kWorld, 7, 1000, mSend, 100, 101);
    recv(a, 1, 0, kWorld, 7, 1000, mRecv, 101, 102);
    send(a, 2, 3, kWorld, 7, 1000, mSend, 100, 101);
    recv(a, 2, 1, kWorld, 7, 1000, mRecv, 101, 102);
    recv(a, 3, 2, kWorld, 7, 1000, mRecv, 101, 102);
    send(a, 3, 0, kWorld, 7, 1000, mSend, 400, 401);

    // Second 1: MPI_Allreduce on WORLD, R2 arrives late
    allreduce(a, 0, 1000, 1201);
    allreduce(a, 1, 1000, 1201);
    allreduce(a, 3, 1000, 1201);
    allreduce(a, 2, 1200, 1201);

    // Second 2
    send(a, 1, 2, kWorld, 9, 500, QStringLiteral("MPI_Ssend"), 2000, 2301);
    recv(a, 2, 1, kWorld, 9, 500, mRecv, 2300, 2301);
    send(a, 3, 2, kSub, 5, 200, mSend, 2050, 2051);
    recv(a, 2, 3, kSub, 5, 200, mRecv, 2100, 2101);
    send(a, 3, 0, kWorld, 11, 100, mSend, 2400, 2401); // never received
}

void verifyRing4(const TraceSummary &s)
{
    const QString p2p = QStringLiteral("p2p");
    const QString coll = QStringLiteral("collective");
    const QString nA = QStringLiteral("nodeA");
    const QString nB = QStringLiteral("nodeB");

    QCOMPARE(s.seconds, 3);
    QCOMPARE(s.unmatchedMessages, 1);
    QCOMPARE(s.rankCount, 4);
    QMap<QString, QVector<int>> nodes;
    nodes.insert(nA, {0, 1});
    nodes.insert(nB, {2, 3});
    QCOMPARE(s.nodes, nodes);
    QCOMPARE(s.rows.size(), 3);
    QCOMPARE(s.details.size(), 3);

    // Bucket table
    QCOMPARE(s.rows[0].size(), 4);
    checkRow(s.rows[0], 0, p2p, nA, 1000, 1000, 0.299, 0, 0.302);
    checkRow(s.rows[0], 1, p2p, nA, 1000, 1000, 0, 0, 0.002);
    checkRow(s.rows[0], 2, p2p, nB, 1000, 1000, 0, 0, 0.002);
    checkRow(s.rows[0], 3, p2p, nB, 1000, 1000, 0, 0, 0.002);

    QCOMPARE(s.rows[1].size(), 4);
    checkRow(s.rows[1], 0, coll, nA, 64, 64, 0.200, 0, 0.201);
    checkRow(s.rows[1], 1, coll, nA, 64, 64, 0.200, 0, 0.201);
    checkRow(s.rows[1], 2, coll, nB, 64, 64, 0, 0, 0.001);
    checkRow(s.rows[1], 3, coll, nB, 64, 64, 0.200, 0, 0.201);

    QCOMPARE(s.rows[2].size(), 3);
    checkRow(s.rows[2], 1, p2p, nA, 500, 0, 0, 0.300, 0.301);
    checkRow(s.rows[2], 2, p2p, nB, 0, 700, 0, 0, 0.002);
    checkRow(s.rows[2], 3, p2p, nB, 300, 0, 0, 0, 0.002);

    // Detail rows
    QCOMPARE(s.details[0].size(), 8);
    for (int r = 0; r < 4; ++r) {
        checkP2PDetail(s.details[0], QStringLiteral("MPI_Send"), r, (r + 1) % 4, 1000, 0);
        checkP2PDetail(s.details[0], QStringLiteral("MPI_Recv"), r, (r + 3) % 4, 0, 1000);
    }

    QCOMPARE(s.details[1].size(), 4);
    for (int r = 0; r < 4; ++r) {
        const DetailRow *d = findDetail(s.details[1], QStringLiteral("MPI_Allreduce"), r, -1);
        QVERIFY(d);
        QCOMPARE(d->commType, coll);
        QCOMPARE(d->collPartners, QByteArray(1, char(0x0F & ~(1 << r))));
        QCOMPARE(d->sendBytes, qint64(64));
        QCOMPARE(d->recvBytes, qint64(64));
    }

    QCOMPARE(s.details[2].size(), 5);
    checkP2PDetail(s.details[2], QStringLiteral("MPI_Ssend"), 1, 2, 500, 0);
    checkP2PDetail(s.details[2], QStringLiteral("MPI_Recv"), 2, 1, 0, 500);
    checkP2PDetail(s.details[2], QStringLiteral("MPI_Send"), 3, 2, 200, 0);
    checkP2PDetail(s.details[2], QStringLiteral("MPI_Recv"), 2, 3, 0, 200);
    checkP2PDetail(s.details[2], QStringLiteral("MPI_Send"), 3, 0, 100, 0);

    // Matrices built from the detail rows the way the DB path does
    Matrix ps, pr, cs, cr;
    const Matrix zero(4, QVector<qint64>(4, 0));

    buildMatrices(s.details[0], ps, pr, cs, cr);
    Matrix eps = zero, epr = zero;
    eps[0][1] = eps[1][2] = eps[2][3] = eps[3][0] = 1000;
    epr[1][0] = epr[2][1] = epr[3][2] = epr[0][3] = 1000;
    QCOMPARE(ps, eps);
    QCOMPARE(pr, epr);
    QCOMPARE(cs, zero);
    QCOMPARE(cr, zero);

    buildMatrices(s.details[1], ps, pr, cs, cr);
    Matrix ecs = zero;
    for (int r = 0; r < 4; ++r)
        for (int p = 0; p < 4; ++p)
            if (p != r)
                ecs[r][p] = 21;
    QCOMPARE(ps, zero);
    QCOMPARE(pr, zero);
    QCOMPARE(cs, ecs);
    QCOMPARE(cr, ecs); // coll_recv[p][r] = 21 for all p != r (symmetric here)

    buildMatrices(s.details[2], ps, pr, cs, cr);
    eps = zero;
    epr = zero;
    eps[1][2] = 500;
    eps[3][2] = 200;
    eps[3][0] = 100;
    epr[2][1] = 500;
    epr[2][3] = 200;
    QCOMPARE(ps, eps);
    QCOMPARE(pr, epr);
    QCOMPARE(cs, zero);
    QCOMPARE(cr, zero);
}
