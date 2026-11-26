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

// Rank-level graph built by hand: `ranks` ranks, `perNode` per compute node, P2P edges
// (from, to, bytes, wait seconds) and optional collective waiting per rank (one communicator).
CommGraph handGraph(int ranks, int perNode, const QVector<std::tuple<int, int, quint64, double>> &p2p,
                    const QVector<double> &collectiveWait = {})
{
    CommGraph g;
    for (int r = 0; r < ranks; ++r) {
        GraphNode n;
        n.kind = GraphNode::Kind::Rank;
        n.rank = r;
        n.group = r / perNode;
        n.node = QStringLiteral("n%1").arg(n.group);
        if (g.groups.size() <= n.group)
            g.groups.append(n.node);
        g.nodes.append(n);
    }
    g.rankNodeCount = ranks;
    for (const auto &[from, to, bytes, wait] : p2p) {
        GraphEdge e;
        e.from = from;
        e.to = to;
        e.bytes = bytes;
        e.messages = 1;
        e.waitSeconds = wait;
        g.edges.append(e);
    }
    if (!collectiveWait.isEmpty()) {
        GraphNode c;
        c.kind = GraphNode::Kind::Communicator;
        c.comm = 0;
        c.members = ranks;
        c.group = -1;
        g.nodes.append(c);
        for (int r = 0; r < ranks; ++r) {
            GraphEdge e;
            e.kind = GraphEdge::Kind::Collective;
            e.from = r;
            e.to = ranks;
            e.messages = 1;
            e.waitSeconds = collectiveWait.value(r);
            g.edges.append(e);
        }
    }
    return g;
}

// Periodic 2D stencil on a cols x rows grid (ranks row by row).
CommGraph stencil(int cols, int rows, int perNode)
{
    QVector<std::tuple<int, int, quint64, double>> p2p;
    for (int r = 0; r < cols * rows; ++r) {
        const int x = r % cols, y = r / cols;
        for (const auto &[dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int to = ((y + dy + rows) % rows) * cols + (x + dx + cols) % cols;
            p2p.append({r, to, 1000, 0.0});
        }
    }
    return handGraph(cols * rows, perNode, p2p);
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

    void mergeCommunicatorsSumsStars()
    {
        CommGraph g;
        g.groups = {QStringLiteral("n")};
        for (int r = 0; r < 3; ++r) {
            GraphNode n;
            n.rank = r;
            g.nodes.append(n);
        }
        g.rankNodeCount = 3;
        for (quint64 c : {7, 9}) {
            GraphNode n;
            n.kind = GraphNode::Kind::Communicator;
            n.comm = c;
            n.members = 3;
            n.group = -1;
            g.nodes.append(n);
        }
        auto add = [&](GraphEdge::Kind k, int from, int to, quint64 bytes, double wait) {
            GraphEdge e;
            e.kind = k;
            e.from = from;
            e.to = to;
            e.bytes = bytes;
            e.messages = 1;
            e.waitSeconds = wait;
            g.edges.append(e);
        };
        add(GraphEdge::Kind::P2P, 0, 1, 10, 0.1);
        add(GraphEdge::Kind::Collective, 0, 3, 100, 0.5);
        add(GraphEdge::Kind::Collective, 0, 4, 200, 0.25);
        add(GraphEdge::Kind::Collective, 2, 4, 50, 0);
        const CommGraph m = mergeCommunicators(g);
        QCOMPARE(m.nodes.size(), 4);
        QCOMPARE(m.rankNodeCount, 3);
        QCOMPARE(m.nodes[3].comm, kMergedCommunicators);
        QCOMPARE(m.nodes[3].members, 2);
        QCOMPARE(m.edges.size(), 3);
        QCOMPARE(findEdge(m, 0, 1)->bytes, quint64(10));
        const GraphEdge *c0 = findEdge(m, 0, 3, GraphEdge::Kind::Collective);
        QVERIFY(c0);
        QCOMPARE(c0->bytes, quint64(300));
        QCOMPARE(c0->messages, 2);
        QCOMPARE(c0->waitSeconds, 0.75);
        QCOMPARE(findEdge(m, 2, 3, GraphEdge::Kind::Collective)->bytes, quint64(50));
        // One communicator: unchanged.
        QCOMPARE(mergeCommunicators(m).nodes.size(), 4);
    }

    // Equal weights under a limit: spread over the ring, not only the first ranks.
    void selectEdgesSpreadsTies()
    {
        CommGraph g;
        g.rankNodeCount = 256;
        g.nodes.resize(256);
        for (int r = 0; r < 256; ++r) {
            GraphEdge e;
            e.from = r;
            e.to = (r + 1) % 256;
            e.bytes = 64;
            g.edges.append(e);
        }
        const QVector<int> sel = selectEdges(g, GraphWeight::Bytes, 64, -1);
        QCOMPARE(sel.size(), 64);
        int quarter[4] = {0, 0, 0, 0};
        for (int i : sel)
            ++quarter[g.edges[i].from / 64];
        for (int q : quarter)
            QVERIFY2(q >= 6, qPrintable(QString::number(q))); // every quarter of the ring gets some
        QCOMPARE(selectEdges(g, GraphWeight::Bytes, 64, -1), sel); // stable
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

    // Pattern detection on rank offsets: ring, pipeline, 2D stencil, all-to-all, irregular.
    void detectsPatterns()
    {
        QVector<std::tuple<int, int, quint64, double>> ring, pipe, all, irregular;
        for (int r = 0; r < 16; ++r) {
            ring.append({r, (r + 1) % 16, 10, 0.0});
            ring.append({r, (r + 15) % 16, 10, 0.0});
            pipe.append({r, (r + 1) % 16, 10, 0.0});
            for (int o = 0; o < 16; ++o)
                if (o != r)
                    all.append({r, o, 10, 0.0});
            irregular.append({r, (r * 7 + 3) % 16 == r ? (r + 2) % 16 : (r * 7 + 3) % 16, 10, 0.0});
        }
        QCOMPARE(detectPattern(handGraph(16, 4, ring)).kind, CommPattern::Kind::Ring);
        const CommPattern p = detectPattern(handGraph(16, 4, pipe));
        QCOMPARE(p.kind, CommPattern::Kind::Pipeline);
        QCOMPARE(p.offsets, QVector<int>{1});
        QCOMPARE(detectPattern(handGraph(16, 4, all)).kind, CommPattern::Kind::AllToAll);
        QCOMPARE(detectPattern(handGraph(16, 4, irregular)).kind, CommPattern::Kind::Irregular);
        QCOMPARE(detectPattern(handGraph(16, 4, {})).kind, CommPattern::Kind::None);

        // 32 x 32 periodic grid (as tools/big-trace): offsets ±1 and ±32 (the wrap-around of a
        // row, -31 / +31, is used by too few ranks to count).
        const CommPattern s = detectPattern(stencil(32, 32, 32));
        QCOMPARE(s.kind, CommPattern::Kind::Stencil2D);
        QCOMPARE(s.gridColumns, 32);
        QCOMPARE(s.offsets, (QVector<int>{-32, -1, 1, 32}));
        QCOMPARE(s.medianPartners, 4);
        QCOMPARE(s.sendingRanks, 1024);
        // Not square: 8 columns x 4 rows.
        QCOMPARE(detectPattern(stencil(8, 4, 8)).gridColumns, 8);
    }

    void findingsNameOutliersAndCauses()
    {
        // Ring of 32 ranks on 4 nodes. Rank 9 sends late to rank 10 (rank 10 waits 50 ms), every
        // other receiver waits 1 ms. Collectives: ranks of node n3 (24..31) wait 0, others 10 ms.
        QVector<std::tuple<int, int, quint64, double>> p2p;
        for (int r = 0; r < 32; ++r)
            p2p.append({r, (r + 1) % 32, 1000, r == 9 ? 0.050 : 0.001});
        QVector<double> coll(32, 0.010);
        for (int r = 24; r < 32; ++r)
            coll[r] = 0;
        const CommGraph g = handGraph(32, 8, p2p, coll);
        const QVector<double> wait = nodeWaiting(g);
        QVERIFY(std::abs(wait[10] - 0.060) < 1e-9);
        QCOMPARE(wait[32], 0.0); // communicator

        const QVector<GraphFinding> f = analyzeCommGraph(g);
        auto find = [&](const QString &kind, int rank = -2) -> const GraphFinding * {
            for (const GraphFinding &x : f)
                if (x.kind == kind && (rank == -2 || x.rank == rank))
                    return &x;
            return nullptr;
        };
        QVERIFY(find(QStringLiteral("pattern")));
        QVERIFY2(find(QStringLiteral("pattern"))->text.contains(QStringLiteral("pipeline")),
                 qPrintable(find(QStringLiteral("pattern"))->text));
        // 4 of 32 ring messages cross a node border.
        const GraphFinding *loc = find(QStringLiteral("locality"));
        QVERIFY(loc);
        QVERIFY(std::abs(loc->value - 28.0 / 32) < 1e-9);
        // Rank 10 waits most: an outlier, and rank 9 is the late sender that causes it.
        QVERIFY(find(QStringLiteral("outlier"), 10));
        QVERIFY(find(QStringLiteral("cause"), 9));
        QVERIFY(!find(QStringLiteral("cause"), 3));
        const GraphFinding *link = find(QStringLiteral("link"));
        QVERIFY(link);
        QVERIFY2(link->text.contains(QStringLiteral("rank 9 → rank 10")), qPrintable(link->text));
        // Node n3 waits least: named as the likely slow node.
        const GraphFinding *imb = find(QStringLiteral("imbalance"));
        QVERIFY(imb);
        QVERIFY2(imb->text.contains(QStringLiteral("n3")) && !imb->text.contains(QStringLiteral("n0")), qPrintable(imb->text));

        // Without waiting: pattern, locality and message sizes only; categories and actions set.
        const QVector<GraphFinding> quiet = analyzeCommGraph(stencil(8, 8, 8));
        QCOMPARE(quiet.size(), 3);
        QCOMPARE(quiet[0].category, QStringLiteral("structure"));
        QCOMPARE(quiet[2].kind, QStringLiteral("size"));
        QCOMPARE(quiet[2].category, QStringLiteral("messages"));
        for (const GraphFinding &x : f)
            QVERIFY2(!x.category.isEmpty(), qPrintable(x.kind));
        QVERIFY(!find(QStringLiteral("cause"), 9)->action.isEmpty());
        QVERIFY(analyzeCommGraph(CommGraph()).isEmpty());
    }

    // Layouts from the data, without knowing the pattern: a grid comes out as a grid, a ring as
    // a ring, tree and star as such (neighbours next to each other); collapsed pictures lose.
    void autoLayoutFollowsStructure()
    {
        // 12 x 8 grid, not periodic.
        QVector<std::tuple<int, int, quint64, double>> grid, ring, tree, star;
        const int cols = 12, n = 96;
        for (int r = 0; r < n; ++r) {
            if (r % cols + 1 < cols)
                grid.append({r, r + 1, 100, 0.0});
            if (r + cols < n)
                grid.append({r, r + cols, 100, 0.0});
            ring.append({r, (r + 1) % n, 100, 0.0});
            if (r > 0) {
                tree.append({r, r & (r - 1), 100, 0.0});
                star.append({r, 0, 100, 0.0});
            }
        }
        const CommGraph g = handGraph(n, 32, grid);
        const LayoutChoice c = autoLayout(g);
        QCOMPARE(c.layout, GraphLayout::Stress);
        QVERIFY2(neighbourhoodPreservation(g, c.positions) > 0.95, qPrintable(QString::number(neighbourhoodPreservation(g, c.positions))));
        QCOMPARE(c.positions.size(), g.nodes.size());
        QCOMPARE(c.scores.size(), 4); // stress, circle, spectral circle, spectral (not torus-shaped)
        // Same structure, same signature (cache key); a changed edge changes it.
        QCOMPARE(backboneSignature(handGraph(n, 32, grid)), c.signature);
        QVERIFY(backboneSignature(handGraph(n, 32, ring)) != c.signature);
        // With a detected stencil the process grid is a candidate as well.
        QVERIFY(autoLayout(g, cols).scores.size() == 5);

        // Ring: a circle (stress or circle), every neighbour next to it.
        const CommGraph rg = handGraph(n, 32, ring);
        QVERIFY(neighbourhoodPreservation(rg, autoLayout(rg).positions) > 0.95);
        // Tree: the tree layout; star: the stress layout (the circle would hide them).
        QCOMPARE(autoLayout(handGraph(n, 32, tree)).layout, GraphLayout::Tree);
        QCOMPARE(autoLayout(handGraph(n, 32, star)).layout, GraphLayout::Stress);
        // Without P2P edges: the circle.
        QCOMPARE(autoLayout(handGraph(n, 32, {})).layout, GraphLayout::Circle);
        // A collapsed picture scores 0 preservation (all nodes on one spot).
        QCOMPARE(neighbourhoodPreservation(g, QVector<QPointF>(n, QPointF(0, 0))), 0.0);
        // Positions stay in the unit square.
        for (const QPointF &p : c.positions)
            QVERIFY(std::abs(p.x()) <= 1.0 && std::abs(p.y()) <= 1.0);
    }

    // The further analyses: late receiver, wait chain, compute imbalance, hub, small messages,
    // rank placement, comparison with a reference.
    void furtherFindings()
    {
        auto find = [](const QVector<GraphFinding> &f, const QString &kind) -> const GraphFinding * {
            for (const GraphFinding &x : f)
                if (x.kind == kind)
                    return &x;
            return nullptr;
        };
        // Pipeline 0 -> 1 -> ... -> 15 (two nodes of 8): rank 0 is late, 1 waits for it and is
        // late itself for 2, ... : a wait chain back to rank 0. Rank 5's receiver (6) is late:
        // 5 waits as a sender (late receiver).
        QVector<std::tuple<int, int, quint64, double>> p2p;
        for (int r = 0; r < 15; ++r)
            p2p.append({r, r + 1, 100, r < 4 ? 0.010 * (r + 1) : 0.0001});
        CommGraph g = handGraph(16, 8, p2p);
        g.windowNs = 1'000'000'000;
        for (GraphEdge &e : g.edges)
            if (e.from == 5)
                e.lateReceiverSeconds = 0.02;
        // Compute time: ranks 12, 13 compute twice as long (spend less in MPI).
        for (int i = 0; i < 16; ++i)
            g.nodes[i].mpiSeconds = (i == 12 || i == 13) ? 0.0 : 0.5;
        const QVector<GraphFinding> f = analyzeCommGraph(g);
        const GraphFinding *chain = find(f, QStringLiteral("chain"));
        QVERIFY(chain);
        QVERIFY2(chain->metric.startsWith(QStringLiteral("0 → 1 → 2")), qPrintable(chain->metric));
        QCOMPARE(chain->rank, 0);
        QVERIFY(chain->links.contains(QPair<int, int>(0, 1)));
        QCOMPARE(chain->category, QStringLiteral("waiting"));
        const GraphFinding *lr = find(f, QStringLiteral("latereceiver"));
        QVERIFY(lr);
        QCOMPARE(lr->rank, 6);
        QVERIFY(lr->ranks.contains(5));
        const GraphFinding *comp = find(f, QStringLiteral("compute"));
        QVERIFY(comp);
        QCOMPARE(comp->category, QStringLiteral("load"));
        QVERIFY(comp->ranks.contains(12) && comp->ranks.contains(13) && comp->ranks.size() == 2);
        QVERIFY(!comp->action.isEmpty());

        // Hub: master-worker, and many small messages.
        QVector<std::tuple<int, int, quint64, double>> star;
        for (int r = 1; r < 16; ++r)
            star.append({0, r, 64, 0.0}), star.append({r, 0, 64, 0.0});
        CommGraph sg = handGraph(16, 8, star);
        for (GraphEdge &e : sg.edges)
            e.messages = 40, e.bytes = 40 * 64;
        const QVector<GraphFinding> sf = analyzeCommGraph(sg);
        const GraphFinding *hub = find(sf, QStringLiteral("hub"));
        QVERIFY(hub);
        QCOMPARE(hub->rank, 0);
        const GraphFinding *size = find(sf, QStringLiteral("size"));
        QVERIFY(size);
        QCOMPARE(size->title, QStringLiteral("Many small messages"));
        QVERIFY(size->action.contains(QStringLiteral("Bundle")));

        // Rank placement: a 8 x 4 grid where every node holds one column (4 ranks): little stays
        // inside; tiles do better.
        QVector<std::tuple<int, int, quint64, double>> grid;
        for (int r = 0; r < 32; ++r) {
            if (r % 8 + 1 < 8)
                grid.append({r, r + 1, 1000, 0.0});
            if (r + 8 < 32)
                grid.append({r, r + 8, 1000, 0.0});
        }
        CommGraph gg = handGraph(32, 4, grid);
        for (int i = 0; i < 32; ++i) {
            gg.nodes[i].group = i % 8; // node = column
            gg.nodes[i].node = QStringLiteral("c%1").arg(i % 8);
        }
        gg.groups.clear();
        for (int c = 0; c < 8; ++c)
            gg.groups << QStringLiteral("c%1").arg(c);
        const MappingSuggestion ms = suggestMapping(gg, 8);
        QVERIFY2(ms.bestShare > ms.currentShare + 0.1, qPrintable(QStringLiteral("%1 %2").arg(ms.currentShare).arg(ms.bestShare)));
        QVERIFY(!ms.how.isEmpty());
        QCOMPARE(ms.nodeOfRank.size(), 32);
        const GraphFinding *map = find(analyzeCommGraph(gg, 8), QStringLiteral("mapping"));
        QVERIFY(map);
        QCOMPARE(map->category, QStringLiteral("structure"));

        // Comparison: the same graph with doubled waiting on 0 -> 1.
        CommGraph later = g;
        for (GraphEdge &e : later.edges)
            if (e.from == 0)
                e.waitSeconds *= 3;
        const QVector<GraphFinding> cmp = compareCommGraphs(later, g);
        QVERIFY(cmp.size() >= 2);
        QCOMPARE(cmp[0].category, QStringLiteral("comparison"));
        QVERIFY(cmp[0].value > 0);
        const GraphFinding *worse = find(cmp, QStringLiteral("compare-link"));
        QVERIFY(worse);
        QCOMPARE(worse->fromRank, 0);
        QCOMPARE(worse->toRank, 1);
        QVERIFY(find(cmp, QStringLiteral("compare-ranks"))->ranks.contains(1));
    }

    // Torus layout: a periodic grid with ranks numbered at random (no pattern to detect) is
    // cut open into a flat grid; a cylinder and a plain grid are not.
    void torusLayoutWithoutNumbering()
    {
        auto torus = [](int cols, int rows, bool periodicY) {
            QVector<int> perm(cols * rows);
            for (int i = 0; i < perm.size(); ++i)
                perm[i] = i;
            quint32 seed = 12345;
            for (int i = int(perm.size()) - 1; i > 0; --i) {
                seed = seed * 1103515245u + 12345u;
                std::swap(perm[i], perm[int((seed >> 8) % quint32(i + 1))]);
            }
            QVector<std::tuple<int, int, quint64, double>> e;
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < cols; ++x) {
                    const int r = y * cols + x;
                    e.append({perm[r], perm[y * cols + (x + 1) % cols], 100, 0.0});
                    if (periodicY || y + 1 < rows)
                        e.append({perm[r], perm[((y + 1) % rows) * cols + x], 100, 0.0});
                }
            return handGraph(cols * rows, 16, e);
        };
        const CommGraph square = torus(16, 16, true);
        const LayoutChoice c = autoLayout(square);
        QCOMPARE(c.layout, GraphLayout::Torus);
        QVERIFY2(neighbourhoodPreservation(square, c.positions) > 0.9,
                 qPrintable(QString::number(neighbourhoodPreservation(square, c.positions))));
        const CommGraph oblong = torus(16, 8, true);
        QCOMPARE(autoLayout(oblong).layout, GraphLayout::Torus);
        QVERIFY(neighbourhoodPreservation(oblong, layoutGraph(oblong, GraphLayout::Torus)) > 0.85);
        // Cylinder (periodic in x only) and the detected-pattern-free plain grid: not a torus.
        QVERIFY(autoLayout(torus(16, 8, false)).layout != GraphLayout::Torus);
        QCOMPARE(layoutName(GraphLayout::Torus), QStringLiteral("torus"));
    }

    // Collective calls rebuilt from a window: ring4's MPI_Allreduce in second 1, where rank 2
    // arrives late (1200 ms) and the others wait for it.
    void collectiveCallsAndLateArrival()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.path());
        QVERIFY(!anchor.isEmpty());
        auto store = build(anchor);
        QVERIFY(store);
        const QVector<CollectiveInstance> inst = collectiveInstances(*store, 1000 * kMs, 2000 * kMs);
        QCOMPARE(inst.size(), 1);
        QCOMPARE(inst[0].function, QStringLiteral("MPI_Allreduce"));
        QCOMPARE(inst[0].ranks.size(), 4);
        QCOMPARE(inst[0].lastRank, 2);
        QCOMPARE(inst[0].lastEnterNs, 1200 * kMs);
        QVERIFY(inst[0].firstEnterNs < inst[0].lastEnterNs);
        QVERIFY(inst[0].totalWaitSeconds > 0);
        QVERIFY(collectiveInstances(*store, 0, 1000 * kMs).isEmpty());

        const QVector<GraphFinding> f = analyzeCollectives(inst, 1000 * kMs, 4);
        QCOMPARE(f.size(), 2);
        QCOMPARE(f[0].kind, QStringLiteral("lateatcollective"));
        QCOMPARE(f[0].category, QStringLiteral("collectives"));
        QCOMPARE(f[0].rank, 2);
        QVERIFY(!f[0].action.isEmpty());
        QCOMPARE(f[1].kind, QStringLiteral("collectiveops"));
        QVERIFY2(f[1].metric.startsWith(QStringLiteral("MPI_Allreduce")), qPrintable(f[1].metric));
        QVERIFY(analyzeCollectives({}, 1000 * kMs, 4).isEmpty());
    }

    // Tree-shaped communication: Auto takes the tree layout, root (most partners) at the top,
    // children one level below their parent.
    void treeLayoutRootOnTop()
    {
        QVector<std::tuple<int, int, quint64, double>> tree;
        for (int r = 1; r < 32; ++r)
            tree.append({r, r & (r - 1), 100, 0.0}); // binomial reduce to rank 0
        const CommGraph g = handGraph(32, 16, tree);
        const LayoutChoice c = autoLayout(g);
        QCOMPARE(c.layout, GraphLayout::Tree);
        const int root = g.indexOfRank(0);
        for (int i = 0; i < 32; ++i)
            if (i != root)
                QVERIFY(c.positions[i].y() > c.positions[root].y());
        // Rank 3's parent is 2, which is one level higher.
        QVERIFY(c.positions[g.indexOfRank(3)].y() > c.positions[g.indexOfRank(2)].y());
        QCOMPARE(layoutName(GraphLayout::Tree), QStringLiteral("tree"));
        // A star (one level) is no tree layout case; a ring is not a tree.
        QVector<std::tuple<int, int, quint64, double>> star, ring;
        for (int r = 1; r < 16; ++r)
            star.append({r, 0, 100, 0.0});
        for (int r = 0; r < 16; ++r)
            ring.append({r, (r + 1) % 16, 100, 0.0});
        QVERIFY(autoLayout(handGraph(16, 8, star)).layout != GraphLayout::Tree);
        QVERIFY(autoLayout(handGraph(16, 8, ring)).layout != GraphLayout::Tree);
    }

    // Critical path of ring4's window: ends at the rank finishing last, jumps at wait states
    // to the late partner; time split adds up.
    void criticalPathOfWindow()
    {
        QTemporaryDir tmp;
        const QString anchor = writeRing4Otf2(tmp.path());
        QVERIFY(!anchor.isEmpty());
        auto store = build(anchor);
        QVERIFY(store);
        const QVector<CollectiveInstance> inst = collectiveInstances(*store, 0, 2500 * kMs);
        const CriticalPath p = criticalPath(*store, 0, 2500 * kMs, inst);
        QVERIFY(!p.ranks.isEmpty());
        QVERIFY(p.lengthSeconds > 0);
        const double sum = p.computeSeconds + p.mpiSeconds + p.transferSeconds;
        QVERIFY2(sum > 0 && sum <= p.lengthSeconds + 1e-6, qPrintable(QStringLiteral("%1 %2").arg(sum).arg(p.lengthSeconds)));
        QCOMPARE(p.jumps.size() + 1 >= p.ranks.size(), true);
        // The allreduce in second 1 waited for rank 2: the path passes rank 2.
        QVERIFY(p.ranks.contains(2));
        const GraphFinding f = criticalPathFinding(p);
        QCOMPARE(f.kind, QStringLiteral("critical"));
        QCOMPARE(f.category, QStringLiteral("critical"));
        QVERIFY(f.metric.contains(QStringLiteral("computation")));
        QVERIFY(!f.action.isEmpty());
        QVERIFY(criticalPathFinding(CriticalPath()).title.isEmpty());
        // Arrival times per member are kept.
        QCOMPARE(inst[0].enters.size(), inst[0].ranks.size());
    }

    // The pattern of a real trace: the ring of writeManyMessagesOtf2 (r -> r + 1).
    void detectsPatternOfTrace()
    {
        QTemporaryDir tmp;
        const QString anchor = writeManyMessagesOtf2(tmp.path(), 24, 4);
        QVERIFY(!anchor.isEmpty());
        auto store = build(anchor);
        QVERIFY(store);
        GraphOptions o;
        const CommGraph g = buildCommGraph(*store, 0, store->traceLengthNs, o);
        QCOMPARE(detectPattern(g).kind, CommPattern::Kind::Pipeline);
    }
};

QTEST_GUILESS_MAIN(TstTraceGraphModel)
#include "tst_trace_graph_model.moc"
