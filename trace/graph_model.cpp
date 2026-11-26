#include "graph_model.h"

#include <QHash>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace trace {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Key of an edge in the hash: from, to, kind (directed or normalised from <= to).
quint64 edgeKey(int from, int to, GraphEdge::Kind kind)
{
    return (quint64(quint32(from)) << 33) | (quint64(quint32(to)) << 1) | (kind == GraphEdge::Kind::Collective ? 1 : 0);
}

} // namespace

int CommGraph::indexOfRank(int rank) const
{
    for (int i = 0; i < rankNodeCount; ++i)
        if (nodes[i].kind == GraphNode::Kind::Rank && nodes[i].rank == rank)
            return i;
    return -1;
}

CommGraph buildCommGraph(const TimelineStore &store, qint64 fromNs, qint64 toNs, const GraphOptions &options)
{
    CommGraph g;
    const bool nodesLevel = options.level == GraphLevel::Nodes;

    // Graph nodes: hosts in order of first appearance, ranks grouped by host (as the timeline).
    QHash<QString, int> groupOf;
    QVector<QVector<int>> rowsOfGroup;
    for (int i = 0; i < store.rows.size(); ++i) {
        const QString &host = store.rows[i].node;
        auto it = groupOf.find(host);
        if (it == groupOf.end()) {
            it = groupOf.insert(host, int(g.groups.size()));
            g.groups.append(host);
            rowsOfGroup.append(QVector<int>());
        }
        rowsOfGroup[*it].append(i);
    }
    QVector<int> nodeOfRow(store.rows.size(), -1);
    for (int grp = 0; grp < g.groups.size(); ++grp) {
        if (nodesLevel && !options.expandedNodes.contains(g.groups[grp])) {
            GraphNode n;
            n.kind = GraphNode::Kind::Node;
            n.node = g.groups[grp];
            n.members = int(rowsOfGroup[grp].size());
            n.group = grp;
            for (int row : rowsOfGroup[grp])
                nodeOfRow[row] = int(g.nodes.size());
            g.nodes.append(n);
            continue;
        }
        for (int row : rowsOfGroup[grp]) {
            GraphNode n;
            n.kind = GraphNode::Kind::Rank;
            n.rank = store.rows[row].rank;
            n.node = g.groups[grp];
            n.group = grp;
            nodeOfRow[row] = int(g.nodes.size());
            g.nodes.append(n);
        }
    }
    g.rankNodeCount = int(g.nodes.size());

    const bool covered = store.complete || (fromNs >= store.fromNs && toNs <= store.toNs);
    if (!covered || toNs <= fromNs)
        return g;

    QHash<quint64, int> edgeIndex;
    QHash<quint64, int> commNode;
    auto edgeFor = [&](int from, int to, GraphEdge::Kind kind) -> GraphEdge & {
        if (!options.directed && kind == GraphEdge::Kind::P2P && to < from)
            std::swap(from, to);
        const quint64 key = edgeKey(from, to, kind);
        auto it = edgeIndex.find(key);
        if (it == edgeIndex.end()) {
            it = edgeIndex.insert(key, int(g.edges.size()));
            GraphEdge e;
            e.kind = kind;
            e.from = from;
            e.to = to;
            g.edges.append(e);
        }
        return g.edges[*it];
    };

    for (int rowIdx = 0; rowIdx < store.rows.size(); ++rowIdx) {
        const RankRow &r = store.rows[rowIdx];
        const int self = nodeOfRow[rowIdx];
        auto first = std::lower_bound(r.activities.cbegin(), r.activities.cend(), fromNs,
                                      [](const Activity &a, qint64 t) { return a.startNs < t; });
        for (auto it = first; it != r.activities.cend() && it->startNs < toNs; ++it) {
            const Activity &a = *it;
            if (a.category == Category::Collective) {
                if (!options.collectives)
                    continue;
                for (int i = 0; i < a.linkCount; ++i) {
                    const CollectiveEntry &c = store.collectives[r.links[a.firstLink + i]];
                    auto cn = commNode.find(c.comm);
                    if (cn == commNode.end()) {
                        GraphNode n;
                        n.kind = GraphNode::Kind::Communicator;
                        n.comm = c.comm;
                        n.members = int(c.members.size());
                        n.group = -1;
                        cn = commNode.insert(c.comm, int(g.nodes.size()));
                        g.nodes.append(n);
                    }
                    GraphEdge &e = edgeFor(self, *cn, GraphEdge::Kind::Collective);
                    e.bytes += c.bytesSent + c.bytesRecv;
                    e.messages += 1;
                    e.waitSeconds += c.lateSender;
                }
                continue;
            }
            if (!options.p2p || (a.category != Category::Send && a.category != Category::Recv))
                continue;
            for (int i = 0; i < a.linkCount; ++i) {
                const qint32 link = r.links[a.firstLink + i];
                // A message to self inside one call (MPI_Sendrecv) is linked twice.
                if (std::find(r.links.cbegin() + a.firstLink, r.links.cbegin() + a.firstLink + i, link)
                        != r.links.cbegin() + a.firstLink + i)
                    continue;
                const Message &m = store.messages[link];
                // Counted once, at the call that sent it.
                if (m.sender != r.rank || m.sendNs < 0 || m.sendNs != a.startNs || m.receiver < 0)
                    continue;
                const int recvRow = store.rowIndexOfRank(m.receiver);
                if (recvRow < 0)
                    continue;
                const int other = nodeOfRow[recvRow];
                if (other == self) {
                    if (g.nodes[self].kind == GraphNode::Kind::Node) {
                        g.nodes[self].internalBytes += m.bytes;
                        g.nodes[self].internalMessages += 1;
                    }
                    continue;
                }
                GraphEdge &e = edgeFor(self, other, GraphEdge::Kind::P2P);
                e.bytes += m.bytes;
                e.messages += 1;
                e.waitSeconds += m.lateSender;
            }
        }
    }

    // Communicators by id, so that their places do not depend on which rank called first.
    const int n = g.rankNodeCount;
    QVector<int> order;
    for (int i = n; i < g.nodes.size(); ++i)
        order.append(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return g.nodes[a].comm < g.nodes[b].comm; });
    QVector<int> newIndex(g.nodes.size());
    for (int i = 0; i < n; ++i)
        newIndex[i] = i;
    QVector<GraphNode> sorted = g.nodes.mid(0, n);
    for (int k = 0; k < order.size(); ++k) {
        newIndex[order[k]] = n + k;
        sorted.append(g.nodes[order[k]]);
    }
    g.nodes = sorted;
    for (GraphEdge &e : g.edges) {
        e.from = newIndex[e.from];
        e.to = newIndex[e.to];
    }
    return g;
}

CommGraph mergeCommunicators(const CommGraph &graph)
{
    const int n = graph.rankNodeCount;
    if (graph.nodes.size() - n <= 1)
        return graph;
    CommGraph g;
    g.groups = graph.groups;
    g.rankNodeCount = n;
    g.nodes = graph.nodes.mid(0, n);
    GraphNode all;
    all.kind = GraphNode::Kind::Communicator;
    all.comm = kMergedCommunicators;
    all.members = int(graph.nodes.size()) - n;
    all.group = -1;
    g.nodes.append(all);
    QHash<int, int> collectiveOf; // rank / host node -> its edge to the merged node
    for (const GraphEdge &e : graph.edges) {
        if (e.kind != GraphEdge::Kind::Collective) {
            g.edges.append(e);
            continue;
        }
        auto it = collectiveOf.find(e.from);
        if (it == collectiveOf.end()) {
            GraphEdge m;
            m.kind = GraphEdge::Kind::Collective;
            m.from = e.from;
            m.to = n;
            it = collectiveOf.insert(e.from, int(g.edges.size()));
            g.edges.append(m);
        }
        GraphEdge &m = g.edges[*it];
        m.bytes += e.bytes;
        m.messages += e.messages;
        m.waitSeconds += e.waitSeconds;
    }
    return g;
}

double edgeWeight(const GraphEdge &edge, GraphWeight weight)
{
    switch (weight) {
    case GraphWeight::Bytes: return double(edge.bytes);
    case GraphWeight::Messages: return double(edge.messages);
    case GraphWeight::Wait: return edge.waitSeconds;
    }
    return 0;
}

QVector<int> selectEdges(const CommGraph &graph, GraphWeight weight, int maxEdges, int focusNode)
{
    QVector<int> order;
    order.reserve(graph.edges.size());
    for (int i = 0; i < graph.edges.size(); ++i)
        if (weight == GraphWeight::Wait || edgeWeight(graph.edges[i], weight) > 0)
            order.append(i);
    // Heaviest first. Ties by a fixed scrambled order of the edge's ends, so that a limit cuts
    // equal edges all over the graph instead of keeping only those of the first ranks.
    auto spread = [&](int i) {
        const GraphEdge &e = graph.edges[i];
        return (quint32(e.from) * 2654435761u) ^ (quint32(e.to) * 2246822519u) ^ quint32(e.kind);
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const double wa = edgeWeight(graph.edges[a], weight);
        const double wb = edgeWeight(graph.edges[b], weight);
        if (wa != wb)
            return wa > wb;
        const quint32 sa = spread(a), sb = spread(b);
        return sa != sb ? sa < sb : a < b;
    });
    if (maxEdges <= 0 || order.size() <= maxEdges)
        return order;
    QVector<int> out = order.mid(0, maxEdges);
    if (focusNode >= 0) {
        for (qsizetype i = maxEdges; i < order.size(); ++i) {
            const GraphEdge &e = graph.edges[order[i]];
            if (e.from == focusNode || e.to == focusNode)
                out.append(order[i]);
        }
    }
    return out;
}

QVector<double> nodeWaiting(const CommGraph &graph)
{
    QVector<double> wait(graph.nodes.size(), 0.0);
    for (const GraphEdge &e : graph.edges)
        wait[e.kind == GraphEdge::Kind::P2P ? e.to : e.from] += e.waitSeconds;
    for (int i = graph.rankNodeCount; i < wait.size(); ++i)
        wait[i] = 0;
    return wait;
}

CommPattern detectPattern(const CommGraph &g)
{
    CommPattern pat;
    const int n = g.rankNodeCount;
    if (n < 2)
        return pat;
    // Rank number of each graph node; offsets in rank numbers (graph order may differ).
    QVector<QSet<int>> partners(n);
    for (const GraphEdge &e : g.edges) {
        if (e.kind != GraphEdge::Kind::P2P || e.from >= n || e.to >= n)
            continue;
        if (g.nodes[e.from].kind != GraphNode::Kind::Rank || g.nodes[e.to].kind != GraphNode::Kind::Rank)
            continue;
        partners[e.from].insert(g.nodes[e.to].rank);
    }
    QMap<int, int> offsetCount; // signed offset -> number of sending ranks using it
    QVector<int> partnerCounts;
    for (int i = 0; i < n; ++i) {
        if (partners[i].isEmpty())
            continue;
        partnerCounts.append(int(partners[i].size()));
        const int self = g.nodes[i].rank;
        for (int other : std::as_const(partners[i])) {
            int d = ((other - self) % n + n) % n;
            if (d > n / 2)
                d -= n;
            offsetCount[d] += 1;
        }
    }
    pat.sendingRanks = int(partnerCounts.size());
    if (partnerCounts.isEmpty())
        return pat;
    std::sort(partnerCounts.begin(), partnerCounts.end());
    pat.medianPartners = partnerCounts[partnerCounts.size() / 2];
    for (auto it = offsetCount.cbegin(); it != offsetCount.cend(); ++it)
        if (2 * it.value() >= pat.sendingRanks)
            pat.offsets.append(it.key());

    if (n > 4 && 2 * pat.medianPartners >= n - 1) {
        pat.kind = CommPattern::Kind::AllToAll;
        return pat;
    }
    QSet<int> abs;
    for (int d : std::as_const(pat.offsets))
        abs.insert(std::abs(d));
    const bool covered = !pat.offsets.isEmpty() && pat.offsets.size() >= pat.medianPartners;
    if (covered && abs == QSet<int>{1}) {
        pat.kind = pat.offsets.size() == 1 ? CommPattern::Kind::Pipeline : CommPattern::Kind::Ring;
        return pat;
    }
    if (covered && abs.size() == 2 && abs.contains(1)) {
        abs.remove(1);
        const int k = *abs.cbegin();
        if (k > 1 && k <= n / 2) {
            pat.kind = CommPattern::Kind::Stencil2D;
            pat.gridColumns = k;
            return pat;
        }
    }
    pat.kind = CommPattern::Kind::Irregular;
    return pat;
}

namespace {

QString bytesText(double bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1024 && u < 4) {
        bytes /= 1024;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(bytes, 0, 'f', u == 0 ? 0 : 1).arg(QLatin1String(units[u]));
}

QString msText(double seconds)
{
    const double ms = seconds * 1000;
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', ms >= 100 ? 0 : ms >= 1 ? 2 : 3);
}

QString offsetsText(const QVector<int> &offsets)
{
    QStringList parts;
    for (int d : offsets)
        parts << (d > 0 ? QStringLiteral("+%1").arg(d) : QString::number(d));
    return parts.join(QStringLiteral(", "));
}

} // namespace

QVector<GraphFinding> analyzeCommGraph(const CommGraph &g)
{
    QVector<GraphFinding> out;
    const int n = g.rankNodeCount;
    if (n == 0 || g.edges.isEmpty())
        return out;

    // 1. Pattern.
    const CommPattern pat = detectPattern(g);
    GraphFinding f;
    f.kind = QStringLiteral("pattern");
    switch (pat.kind) {
    case CommPattern::Kind::None:
        f.text = QStringLiteral("Pattern: no point-to-point messages, only collectives");
        break;
    case CommPattern::Kind::Ring:
        f.text = QStringLiteral("Pattern: ring – every rank exchanges with its neighbours ±1");
        break;
    case CommPattern::Kind::Pipeline:
        f.text = QStringLiteral("Pattern: pipeline – every rank sends to rank %1").arg(offsetsText(pat.offsets));
        break;
    case CommPattern::Kind::Stencil2D:
        f.text = QStringLiteral("Pattern: 2D stencil / halo exchange – partners at offsets %1, i.e. a grid %2 × %3")
                     .arg(offsetsText(pat.offsets)).arg(pat.gridColumns).arg((n + pat.gridColumns - 1) / pat.gridColumns);
        break;
    case CommPattern::Kind::AllToAll:
        f.text = QStringLiteral("Pattern: all-to-all – a rank talks to %1 of %2 other ranks").arg(pat.medianPartners).arg(n - 1);
        break;
    case CommPattern::Kind::Irregular:
        f.text = pat.offsets.isEmpty()
            ? QStringLiteral("Pattern: irregular – a rank talks to %1 partners (median), no common offset").arg(pat.medianPartners)
            : QStringLiteral("Pattern: irregular – a rank talks to %1 partners (median), most at offsets %2")
                  .arg(pat.medianPartners).arg(offsetsText(pat.offsets));
        break;
    }
    out.append(f);

    // 2. Locality: P2P bytes inside / between compute nodes (ranks of different groups).
    double inside = 0, between = 0;
    for (const GraphEdge &e : g.edges) {
        if (e.kind != GraphEdge::Kind::P2P || e.from >= n || e.to >= n)
            continue;
        (g.nodes[e.from].group == g.nodes[e.to].group ? inside : between) += double(e.bytes);
    }
    if (g.groups.size() > 1 && inside + between > 0) {
        GraphFinding l;
        l.kind = QStringLiteral("locality");
        l.value = inside / (inside + between);
        l.text = QStringLiteral("Locality: %1 % of the P2P volume (%2) stays inside the compute nodes, %3 crosses the network")
                     .arg(qRound(100 * l.value)).arg(bytesText(inside + between)).arg(bytesText(between));
        out.append(l);
    }

    // 3. Waiting per rank: mean, outliers (> mean + 2 sigma and > 1.5 x mean), share at collectives.
    const QVector<double> wait = nodeWaiting(g);
    double sum = 0, sum2 = 0, coll = 0;
    int counted = 0;
    for (int i = 0; i < n; ++i) {
        if (g.nodes[i].kind != GraphNode::Kind::Rank)
            continue;
        sum += wait[i];
        sum2 += wait[i] * wait[i];
        ++counted;
    }
    for (const GraphEdge &e : g.edges)
        if (e.kind == GraphEdge::Kind::Collective)
            coll += e.waitSeconds;
    if (counted > 0 && sum > 0) {
        const double mean = sum / counted;
        const double sigma = std::sqrt(std::max(0.0, sum2 / counted - mean * mean));
        GraphFinding w;
        w.kind = QStringLiteral("waiting");
        w.value = mean;
        w.text = QStringLiteral("Waiting: %1 per rank on average, %2 % of it in collectives (wait at collective), %3 % in P2P (late sender)")
                     .arg(msText(mean)).arg(qRound(100 * coll / sum)).arg(100 - qRound(100 * coll / sum));
        out.append(w);
        QVector<int> order;
        for (int i = 0; i < n; ++i)
            if (g.nodes[i].kind == GraphNode::Kind::Rank && wait[i] > mean + 2 * sigma && wait[i] > 1.5 * mean)
                order.append(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return wait[a] > wait[b]; });
        for (int k = 0; k < std::min<qsizetype>(3, order.size()); ++k) {
            const int i = order[k];
            GraphFinding o;
            o.kind = QStringLiteral("waiting");
            o.rank = g.nodes[i].rank;
            o.value = wait[i];
            o.text = QStringLiteral("Outlier: rank %1 (%2) waits %3 – %4× the mean")
                         .arg(o.rank).arg(g.nodes[i].node).arg(msText(wait[i])).arg(wait[i] / mean, 0, 'f', 1);
            out.append(o);
        }
        if (order.size() > 3) {
            GraphFinding more;
            more.kind = QStringLiteral("waiting");
            more.text = QStringLiteral("… %1 more ranks wait far more than the mean").arg(order.size() - 3);
            out.append(more);
        }
    }

    // 3b. Compute nodes whose ranks wait much less than the rest: in a bulk-synchronous program
    // the others wait for them, i.e. they are probably the slow ones (load imbalance per node).
    if (g.groups.size() > 2 && sum > 0) {
        QVector<double> groupWait(g.groups.size(), 0.0);
        QVector<int> groupRanks(g.groups.size(), 0);
        for (int i = 0; i < n; ++i) {
            const int grp = g.nodes[i].group;
            if (g.nodes[i].kind != GraphNode::Kind::Rank || grp < 0 || grp >= groupWait.size())
                continue;
            groupWait[grp] += wait[i];
            groupRanks[grp] += 1;
        }
        double gsum = 0;
        int gcount = 0;
        for (int k = 0; k < groupWait.size(); ++k)
            if (groupRanks[k] > 0) {
                groupWait[k] /= groupRanks[k];
                gsum += groupWait[k];
                ++gcount;
            }
        const double gmean = gcount > 0 ? gsum / gcount : 0;
        double gvar = 0;
        for (int k = 0; k < groupWait.size(); ++k)
            if (groupRanks[k] > 0)
                gvar += (groupWait[k] - gmean) * (groupWait[k] - gmean);
        const double gsigma = gcount > 0 ? std::sqrt(gvar / gcount) : 0;
        QStringList slow;
        double slowWait = 0;
        // Clearly below the others: 1.5 sigma under the mean and at most 85 % of it.
        for (int k = 0; k < groupWait.size(); ++k)
            if (groupRanks[k] > 0 && gmean - groupWait[k] > 1.5 * gsigma && groupWait[k] < 0.85 * gmean) {
                slow << g.groups[k];
                slowWait += groupWait[k];
            }
        if (!slow.isEmpty() && slow.size() <= gcount / 2) {
            GraphFinding s;
            s.kind = QStringLiteral("imbalance");
            s.value = slowWait / slow.size();
            QStringList shown = slow.mid(0, 8);
            if (slow.size() > 8)
                shown << QStringLiteral("…");
            s.text = QStringLiteral("Imbalance: the ranks of %1 wait only %2 on average (others %3) – the others probably wait for them, these nodes are likely slower")
                         .arg(shown.join(QStringLiteral(", "))).arg(msText(s.value)).arg(msText(gmean));
            out.append(s);
        }
    }

    // 4. Cause: senders whose messages arrived late (sum of late-sender time they caused).
    QVector<double> caused(n, 0.0);
    double causedSum = 0;
    for (const GraphEdge &e : g.edges) {
        if (e.kind != GraphEdge::Kind::P2P || e.from >= n)
            continue;
        caused[e.from] += e.waitSeconds;
        causedSum += e.waitSeconds;
    }
    if (causedSum > 0) {
        QVector<int> order;
        for (int i = 0; i < n; ++i)
            if (caused[i] > 0)
                order.append(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return caused[a] > caused[b]; });
        const double mean = causedSum / std::max(1, counted);
        for (int k = 0; k < std::min<qsizetype>(2, order.size()); ++k) {
            const int i = order[k];
            if (caused[i] < 2 * mean)
                break;
            GraphFinding c;
            c.kind = QStringLiteral("cause");
            c.rank = g.nodes[i].rank;
            c.value = caused[i];
            c.text = QStringLiteral("Late sender: rank %1 (%2) makes its receivers wait %3 in total – %4× the mean, likely slower than its neighbours")
                         .arg(c.rank).arg(g.nodes[i].node).arg(msText(caused[i])).arg(caused[i] / mean, 0, 'f', 1);
            out.append(c);
        }
        // Link with the most waiting.
        int best = -1;
        for (int i = 0; i < g.edges.size(); ++i)
            if (g.edges[i].kind == GraphEdge::Kind::P2P
                && (best < 0 || g.edges[i].waitSeconds > g.edges[best].waitSeconds))
                best = i;
        if (best >= 0 && g.edges[best].waitSeconds > 0) {
            const GraphEdge &e = g.edges[best];
            GraphFinding l;
            l.kind = QStringLiteral("link");
            l.rank = g.nodes[e.to].rank;
            l.value = e.waitSeconds;
            l.text = QStringLiteral("Most waiting on one link: rank %1 → rank %2, %3 in %4 messages")
                         .arg(g.nodes[e.from].rank).arg(g.nodes[e.to].rank).arg(msText(e.waitSeconds)).arg(e.messages);
            out.append(l);
        }
    }
    return out;
}

QVector<QPointF> layoutGraph(const CommGraph &graph, GraphLayout layout)
{
    QVector<QPointF> pos(graph.nodes.size());
    const int n = graph.rankNodeCount;
    const int comms = int(graph.nodes.size()) - n;

    if (layout == GraphLayout::Grid) {
        const int cols = std::max(1, int(std::ceil(std::sqrt(double(n)))));
        const int rows = std::max(1, (n + cols - 1) / cols);
        // Grid in x [-0.9, 0.9], y [-0.9, 0.6] (or to 0.9 without communicators).
        const double yMax = comms > 0 ? 0.6 : 0.9;
        for (int i = 0; i < n; ++i) {
            const int c = i % cols;
            const int r = i / cols;
            const double x = cols > 1 ? -0.9 + 1.8 * c / (cols - 1) : 0.0;
            const double y = rows > 1 ? -0.9 + (yMax + 0.9) * r / (rows - 1) : 0.0;
            pos[i] = QPointF(x, y);
        }
        for (int k = 0; k < comms; ++k) {
            const double x = comms > 1 ? -0.9 + 1.8 * k / (comms - 1) : 0.0;
            pos[n + k] = QPointF(x, 0.9);
        }
        return pos;
    }

    // Circle, starting at the top and going clockwise.
    for (int i = 0; i < n; ++i) {
        const double a = -kPi / 2 + 2 * kPi * i / std::max(1, n);
        pos[i] = QPointF(std::cos(a), std::sin(a));
    }
    for (int k = 0; k < comms; ++k) {
        if (comms == 1) {
            pos[n] = QPointF(0, 0);
            break;
        }
        const double a = -kPi / 2 + 2 * kPi * k / comms;
        pos[n + k] = QPointF(0.25 * std::cos(a), 0.25 * std::sin(a));
    }
    return pos;
}

} // namespace trace
