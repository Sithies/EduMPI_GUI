#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <limits>

#include "graph_model.h"
#include "otf2_fixture.h"
#include "otf2_trace_reader.h"
#include "timeline_builder.h"
#include "trace_aggregator.h"

using namespace trace;

namespace {

constexpr qint64 kMs = 1'000'000;
constexpr qint64 kSec = 1'000'000'000;
constexpr qint64 kAll = std::numeric_limits<qint64>::max();

std::shared_ptr<TimelineStore> build(const QString &anchor, qint64 from = 0, qint64 to = kAll)
{
    TraceAggregator agg;
    TimelineBuilder builder(agg, from, to, true, 2);
    TraceMeta meta;
    QString error;
    if (!readOtf2(anchor, builder, &meta, &error)) {
        qWarning("%s", qPrintable(error));
        return {};
    }
    agg.finish(meta.traceLengthNs);
    return builder.finish();
}

const GraphEdge *findEdge(const CommGraph &g, int from, int to, GraphEdge::Kind kind = GraphEdge::Kind::P2P)
{
    for (const GraphEdge &e : g.edges)
        if (e.from == from && e.to == to && e.kind == kind)
            return &e;
    return nullptr;
}

int commIndex(const CommGraph &g, quint64 comm)
{
    for (int i = g.rankNodeCount; i < g.nodes.size(); ++i)
        if (g.nodes[i].comm == comm)
            return i;
    return -1;
}

// Complete store with `ranks` ranks (two per node), a ring of one message per rank at
// [r ms, r ms + 1 us] (bytes = 100 * (r + 1)) and an all-rank collective on comm 1 at 500 ms.
std::shared_ptr<TimelineStore> syntheticRing(int ranks)
{
    auto s = std::make_shared<TimelineStore>();
    s->traceLengthNs = kSec;
    s->toNs = kSec;
    s->complete = true;
    s->functions = {QStringLiteral("MPI_Send"), QStringLiteral("MPI_Recv"), QStringLiteral("MPI_Allreduce")};
    s->rows.resize(ranks);
    QVector<int> all;
    for (int r = 0; r < ranks; ++r)
        all.append(r);
    for (int r = 0; r < ranks; ++r) {
        s->rows[r].rank = r;
        s->rows[r].node = QStringLiteral("node%1").arg(r / 2);
        s->collectives.append({r, 1, all, 64, 64, r == 0 ? 0.25 : 0.0});
    }
    for (int r = 0; r < ranks; ++r) {
        const int to = (r + 1) % ranks;
        const qint64 t = r * kMs;
        const int msg = int(s->messages.size());
        s->messages.append({r, to, t, t + 2 * kMs, quint64(100 * (r + 1)), 0, 1, 0.001 * r, 0, 0, 1, to});
        RankRow &sender = s->rows[r];
        sender.activities.append({t, t + 1000, 0, qint32(sender.links.size()), 1, 0, Category::Send});
        sender.links.append(msg);
        RankRow &receiver = s->rows[to];
        receiver.activities.append({t + kMs, t + 2 * kMs, 0, qint32(receiver.links.size()), 1, 1, Category::Recv});
        receiver.links.append(msg);
    }
    for (int r = 0; r < ranks; ++r) {
        RankRow &row = s->rows[r];
        row.activities.append({500 * kMs, 501 * kMs, 0, qint32(row.links.size()), 1, 2, Category::Collective});
        row.links.append(r);
        std::sort(row.activities.begin(), row.activities.end(),
                  [](const Activity &a, const Activity &b) { return a.startNs < b.startNs; });
    }
    return s;
}

} // namespace

class TstTraceGraphModel : public QObject
{
    Q_OBJECT

private slots:
    void ring4RanksAndRing()
    {
        QTemporaryDir tmp;
        auto store = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(store);
        // Second 0: ring 0 -> 1 -> 2 -> 3 -> 0 (1000 B each), R0 waits 299 ms for R3.
        const CommGraph g = buildCommGraph(*store, 0, kSec, {});
        QCOMPARE(g.rankNodeCount, 4);
        QCOMPARE(g.nodes.size(), 4); // no collective in second 0
        QCOMPARE(g.groups, QStringList({QStringLiteral("nodeA"), QStringLiteral("nodeB")}));
        QCOMPARE(g.edges.size(), 4);
        for (int r = 0; r < 4; ++r) {
            const GraphEdge *e = findEdge(g, g.indexOfRank(r), g.indexOfRank((r + 1) % 4));
            QVERIFY2(e, qPrintable(QStringLiteral("edge %1 -> %2").arg(r).arg((r + 1) % 4)));
            QCOMPARE(e->bytes, quint64(1000));
            QCOMPARE(e->messages, 1);
        }
        const GraphEdge *late = findEdge(g, g.indexOfRank(3), g.indexOfRank(0));
        QVERIFY(std::abs(late->waitSeconds - 0.299) < 1e-6);
        QCOMPARE(findEdge(g, g.indexOfRank(0), g.indexOfRank(1))->waitSeconds, 0.0);
    }

    void ring4CollectiveStar()
    {
        QTemporaryDir tmp;
        auto store = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(store);
        // Second 1: MPI_Allreduce on WORLD (comm 1 in the fixture's definitions order), R2 late.
        const CommGraph g = buildCommGraph(*store, kSec, 2 * kSec, {});
        QCOMPARE(g.nodes.size(), 5);
        const int comm = g.rankNodeCount;
        QCOMPARE(g.nodes[comm].kind, GraphNode::Kind::Communicator);
        QCOMPARE(g.nodes[comm].members, 4);
        QCOMPARE(g.nodes[comm].group, -1);
        QCOMPARE(g.edges.size(), 4);
        for (int r = 0; r < 4; ++r) {
            const GraphEdge *e = findEdge(g, g.indexOfRank(r), comm, GraphEdge::Kind::Collective);
            QVERIFY(e);
            QCOMPARE(e->messages, 1);
            QVERIFY(std::abs(e->waitSeconds - (r == 2 ? 0.0 : 0.2)) < 1e-6);
        }

        GraphOptions noColl;
        noColl.collectives = false;
        const CommGraph h = buildCommGraph(*store, kSec, 2 * kSec, noColl);
        QCOMPARE(h.nodes.size(), 4);
        QVERIFY(h.edges.isEmpty());
    }

    void ring4SecondTwoLeavesOutUnmatched()
    {
        QTemporaryDir tmp;
        auto store = build(writeRing4Otf2(tmp.filePath(QStringLiteral("r4"))));
        QVERIFY(store);
        // Second 2: 1 -> 2 (500 B), 3 -> 2 (200 B, SUB), 3 -> 0 never received.
        GraphOptions p2pOnly;
        p2pOnly.collectives = false;
        const CommGraph g = buildCommGraph(*store, 2 * kSec, 3 * kSec, p2pOnly);
        QCOMPARE(g.edges.size(), 2);
        QCOMPARE(findEdge(g, g.indexOfRank(1), g.indexOfRank(2))->bytes, quint64(500));
        QCOMPARE(findEdge(g, g.indexOfRank(3), g.indexOfRank(2))->bytes, quint64(200));
        QVERIFY(!findEdge(g, g.indexOfRank(3), g.indexOfRank(0)));
    }

    void windowCountsSendCallStart()
    {
        auto s = syntheticRing(4);
        // Sends start at 0, 1, 2, 3 ms: [1 ms, 3 ms) holds the sends of ranks 1 and 2.
        GraphOptions o;
        o.collectives = false;
        const CommGraph g = buildCommGraph(*s, kMs, 3 * kMs, o);
        QCOMPARE(g.edges.size(), 2);
        QVERIFY(findEdge(g, 1, 2));
        QVERIFY(findEdge(g, 2, 3));
        // Empty and inverted windows: nodes, no edges.
        QCOMPARE(buildCommGraph(*s, 10 * kMs, 10 * kMs, o).edges.size(), 0);
        QCOMPARE(buildCommGraph(*s, 10 * kMs, 5 * kMs, o).nodes.size(), 4);
    }

    void storeNotCoveringWindowGivesNoEdges()
    {
        auto s = syntheticRing(4);
        s->complete = false;
        s->fromNs = 0;
        s->toNs = 2 * kMs;
        QVERIFY(buildCommGraph(*s, 0, kSec, {}).edges.isEmpty());
        QCOMPARE(buildCommGraph(*s, 0, kSec, {}).nodes.size(), 4);
        QCOMPARE(buildCommGraph(*s, 0, 2 * kMs, {}).edges.size(), 2);
    }

    void undirectedMergesBothDirections()
    {
        auto s = syntheticRing(2); // 0 -> 1 (100 B) and 1 -> 0 (200 B)
        GraphOptions o;
        o.collectives = false;
        QCOMPARE(buildCommGraph(*s, 0, kSec, o).edges.size(), 2);
        o.directed = false;
        const CommGraph g = buildCommGraph(*s, 0, kSec, o);
        QCOMPARE(g.edges.size(), 1);
        QCOMPARE(g.edges[0].from, 0);
        QCOMPARE(g.edges[0].to, 1);
        QCOMPARE(g.edges[0].bytes, quint64(300));
        QCOMPARE(g.edges[0].messages, 2);
    }

    void nodesLevelAggregatesHosts()
    {
        auto s = syntheticRing(6); // node0 = {0,1}, node1 = {2,3}, node2 = {4,5}
        GraphOptions o;
        o.level = GraphLevel::Nodes;
        const CommGraph g = buildCommGraph(*s, 0, kSec, o);
        QCOMPARE(g.rankNodeCount, 3);
        QCOMPARE(g.nodes[0].kind, GraphNode::Kind::Node);
        QCOMPARE(g.nodes[0].members, 2);
        // Inside node0: 0 -> 1 (100 B); between hosts: 1 -> 2, 3 -> 4, 5 -> 0.
        QCOMPARE(g.nodes[0].internalBytes, quint64(100));
        QCOMPARE(g.nodes[0].internalMessages, 1);
        QCOMPARE(findEdge(g, 0, 1)->bytes, quint64(200));
        QCOMPARE(findEdge(g, 1, 2)->bytes, quint64(400));
        QCOMPARE(findEdge(g, 2, 0)->bytes, quint64(600));
        QCOMPARE(g.indexOfRank(0), -1);
        // Collective: one star edge per host, two calls each.
        const int comm = commIndex(g, 1);
        QVERIFY(comm >= 0);
        const GraphEdge *e = findEdge(g, 0, comm, GraphEdge::Kind::Collective);
        QVERIFY(e);
        QCOMPARE(e->messages, 2);
        QCOMPARE(e->bytes, quint64(256));
        QVERIFY(std::abs(e->waitSeconds - 0.25) < 1e-9);
    }

    void communicatorsSortedById()
    {
        auto s = syntheticRing(4);
        // Rank 0's collective moves to comm 9: comm 9 is met first, still placed after comm 1.
        s->collectives[0].comm = 9;
        const CommGraph g = buildCommGraph(*s, 0, kSec, {});
        QCOMPARE(g.nodes.size(), 6);
        QCOMPARE(g.nodes[4].comm, quint64(1));
        QCOMPARE(g.nodes[5].comm, quint64(9));
        QVERIFY(findEdge(g, 0, 5, GraphEdge::Kind::Collective));
        QVERIFY(findEdge(g, 1, 4, GraphEdge::Kind::Collective));
    }

    void selectEdgesByWeightWithFocus()
    {
        auto s = syntheticRing(8);
        GraphOptions o;
        o.collectives = false;
        const CommGraph g = buildCommGraph(*s, 0, kSec, o);
        QCOMPARE(g.edges.size(), 8);
        // Bytes: rank r sends 100 * (r + 1): heaviest are 7, 6, 5.
        QVector<int> top = selectEdges(g, GraphWeight::Bytes, 3, -1);
        QCOMPARE(top.size(), 3);
        QCOMPARE(g.edges[top[0]].from, 7);
        QCOMPARE(g.edges[top[1]].from, 6);
        QCOMPARE(g.edges[top[2]].from, 5);
        // Focus rank 0: its edges 0 -> 1 and 7 -> 0; 7 -> 0 is already in the top 3.
        top = selectEdges(g, GraphWeight::Bytes, 3, 0);
        QCOMPARE(top.size(), 4);
        QCOMPARE(g.edges[top[3]].from, 0);
        // All edges.
        QCOMPARE(selectEdges(g, GraphWeight::Bytes, 0, -1).size(), 8);
        // Wait: rank 0's message has no wait but is still listed (last).
        top = selectEdges(g, GraphWeight::Wait, 0, -1);
        QCOMPARE(top.size(), 8);
        QCOMPARE(g.edges[top.last()].from, 0);
        QCOMPARE(edgeWeight(g.edges[top.first()], GraphWeight::Wait), 0.007);
        QCOMPARE(edgeWeight(g.edges[top.first()], GraphWeight::Messages), 1.0);
    }

    void layouts()
    {
        auto s = syntheticRing(4);
        const CommGraph g = buildCommGraph(*s, 0, kSec, {}); // 4 ranks + comm 1
        const QVector<QPointF> circle = layoutGraph(g, GraphLayout::Circle);
        QCOMPARE(circle.size(), 5);
        QVERIFY(std::abs(circle[0].x()) < 1e-9 && std::abs(circle[0].y() + 1) < 1e-9); // top
        QVERIFY(std::abs(circle[1].x() - 1) < 1e-9 && std::abs(circle[1].y()) < 1e-9);  // right
        QCOMPARE(circle[4], QPointF(0, 0)); // single communicator in the centre
        const QVector<QPointF> grid = layoutGraph(g, GraphLayout::Grid);
        QCOMPARE(grid.size(), 5);
        QCOMPARE(grid[0], QPointF(-0.9, -0.9));
        QCOMPARE(grid[1], QPointF(0.9, -0.9));
        QCOMPARE(grid[3], QPointF(0.9, 0.6));
        QCOMPARE(grid[4], QPointF(0, 0.9));
        for (const QPointF &p : circle + grid)
            QVERIFY(std::abs(p.x()) <= 1 && std::abs(p.y()) <= 1);
    }

    void scalesToThousandRanks()
    {
        auto s = syntheticRing(1024);
        QElapsedTimer t;
        t.start();
        // Sends at r ms: the last ones lie beyond 1 s.
        const CommGraph g = buildCommGraph(*s, 0, 2 * kSec, {});
        const QVector<int> sel = selectEdges(g, GraphWeight::Bytes, 500, 3);
        const QVector<QPointF> pos = layoutGraph(g, GraphLayout::Circle);
        QCOMPARE(g.edges.size(), 1024 + 1024);
        QCOMPARE(sel.size(), 500 + 3); // rank 3's edges (2 -> 3, 3 -> 4, star) are below the top 500
        QCOMPARE(pos.size(), 1025);
        QVERIFY2(t.elapsed() < 500, qPrintable(QString::number(t.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TstTraceGraphModel)
#include "tst_trace_graph_model.moc"
