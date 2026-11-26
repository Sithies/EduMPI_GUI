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
            n.rankCount = n.members;
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
    g.windowNs = std::max<qint64>(0, toNs - fromNs);
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
        // Time in MPI calls overlapping the window.
        {
            double mpi = 0;
            // MPI calls of a rank do not overlap: only the one before the first starting in
            // the window can reach into it.
            auto a0 = std::lower_bound(r.activities.cbegin(), r.activities.cend(), fromNs,
                                       [](const Activity &a, qint64 t) { return a.startNs < t; });
            if (a0 != r.activities.cbegin())
                --a0;
            for (auto it = a0; it != r.activities.cend() && it->startNs < toNs; ++it)
                if (it->endNs > fromNs)
                    mpi += double(std::min(it->endNs, toNs) - std::max(it->startNs, fromNs)) / 1e9;
            g.nodes[self].mpiSeconds += std::max(0.0, mpi);
        }
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
                e.lateReceiverSeconds += m.lateReceiver;
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
    g.windowNs = graph.windowNs;
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

namespace {

// Undirected P2P volume between rank-level graph nodes (i < j): key (i << 32 | j).
QHash<quint64, double> undirectedVolume(const CommGraph &g)
{
    QHash<quint64, double> v;
    const int n = g.rankNodeCount;
    for (const GraphEdge &e : g.edges) {
        if (e.kind != GraphEdge::Kind::P2P || e.from >= n || e.to >= n || e.from == e.to)
            continue;
        const int a = std::min(e.from, e.to), c = std::max(e.from, e.to);
        v[(quint64(quint32(a)) << 32) | quint32(c)] += double(e.bytes);
    }
    return v;
}

double insideShare(const QHash<quint64, double> &vol, const QVector<int> &nodeOf)
{
    double in = 0, all = 0;
    for (auto it = vol.cbegin(); it != vol.cend(); ++it) {
        const int a = int(it.key() >> 32), c = int(it.key() & 0xffffffffu);
        all += it.value();
        if (nodeOf[a] == nodeOf[c])
            in += it.value();
    }
    return all > 0 ? in / all : 0;
}

QString rankList(const QVector<int> &ranks, int max = 4)
{
    QStringList l;
    for (int k = 0; k < std::min<qsizetype>(max, ranks.size()); ++k)
        l << QString::number(ranks[k]);
    if (ranks.size() > max)
        l << QStringLiteral("…");
    return l.join(QStringLiteral(", "));
}

} // namespace

MappingSuggestion suggestMapping(const CommGraph &g, int gridColumns)
{
    MappingSuggestion out;
    const int n = g.rankNodeCount;
    if (n < 4 || g.groups.size() < 2)
        return out;
    QVector<int> current(n);
    QHash<int, int> groupSize;
    for (int i = 0; i < n; ++i) {
        current[i] = g.nodes[i].group;
        groupSize[current[i]] += 1;
    }
    // Node size: the most common one; only uniform-size mappings are suggested.
    int m = 0, best = 0;
    QHash<int, int> sizes;
    for (int sz : std::as_const(groupSize))
        sizes[sz] += 1;
    for (auto it = sizes.cbegin(); it != sizes.cend(); ++it)
        if (it.value() > best) {
            best = it.value();
            m = it.key();
        }
    if (m < 2 || n % m != 0)
        return out;
    const QHash<quint64, double> vol = undirectedVolume(g);
    if (vol.isEmpty())
        return out;
    out.currentShare = insideShare(vol, current);
    out.bestShare = out.currentShare;

    // Graph node of each rank number (grid tiles are by rank number).
    QVector<int> nodeOfRank(n, -1);
    for (int i = 0; i < n; ++i)
        if (g.nodes[i].kind == GraphNode::Kind::Rank && g.nodes[i].rank >= 0 && g.nodes[i].rank < n)
            nodeOfRank[g.nodes[i].rank] = i;
    const bool byRank = !nodeOfRank.contains(-1);

    // (a) Tiles a x b of the process grid.
    if (gridColumns > 1 && byRank && n % gridColumns == 0) {
        const int cols = gridColumns, rows = n / gridColumns;
        for (int a = 1; a <= m; ++a) {
            if (m % a != 0)
                continue;
            const int b = m / a;
            if (cols % a != 0 || rows % b != 0)
                continue;
            QVector<int> assign(n);
            for (int r = 0; r < n; ++r) {
                const int x = r % cols, y = r / cols;
                assign[nodeOfRank[r]] = (y / b) * (cols / a) + x / a;
            }
            const double share = insideShare(vol, assign);
            if (share > out.bestShare + 1e-9) {
                out.bestShare = share;
                out.nodeOfRank = assign;
                out.how = QStringLiteral("blocks of %1 × %2 ranks of the process grid per node").arg(a).arg(b);
            }
        }
    }
    // (b) Greedy: grow each node from the lowest unassigned rank by the strongest connection.
    {
        QVector<QVector<std::pair<int, double>>> adj(n);
        for (auto it = vol.cbegin(); it != vol.cend(); ++it) {
            const int a = int(it.key() >> 32), c = int(it.key() & 0xffffffffu);
            adj[a].append({c, it.value()});
            adj[c].append({a, it.value()});
        }
        QVector<int> assign(n, -1);
        int node = 0;
        for (int seed = 0; seed < n; ++seed) {
            if (assign[seed] >= 0)
                continue;
            QHash<int, double> gain; // unassigned neighbour -> connection to the group
            assign[seed] = node;
            int size = 1;
            for (const auto &[v, w] : adj[seed])
                if (assign[v] < 0)
                    gain[v] += w;
            while (size < m) {
                int pick = -1;
                double pw = -1;
                for (auto it = gain.cbegin(); it != gain.cend(); ++it)
                    if (assign[it.key()] < 0 && (it.value() > pw || (it.value() == pw && it.key() < pick))) {
                        pick = it.key();
                        pw = it.value();
                    }
                if (pick < 0) {
                    for (int r = 0; r < n; ++r)
                        if (assign[r] < 0) {
                            pick = r;
                            break;
                        }
                }
                if (pick < 0)
                    break;
                assign[pick] = node;
                gain.remove(pick);
                ++size;
                for (const auto &[v, w] : adj[pick])
                    if (assign[v] < 0)
                        gain[v] += w;
            }
            ++node;
        }
        const double share = insideShare(vol, assign);
        if (share > out.bestShare + 0.02) {
            out.bestShare = share;
            out.nodeOfRank = assign;
            out.how = QStringLiteral("ranks grouped by who talks to whom, %1 per node").arg(m);
        }
    }
    return out;
}

QVector<GraphFinding> analyzeCommGraph(const CommGraph &g, int gridColumns)
{
    QVector<GraphFinding> out;
    const int n = g.rankNodeCount;
    if (n == 0 || g.edges.isEmpty())
        return out;
    auto finish = [&](GraphFinding f, const QString &line = QString()) {
        f.text = line.isEmpty() ? f.title + QStringLiteral(": ") + (f.metric.isEmpty() ? QString() : f.metric + QStringLiteral(" – ")) + f.detail
                                : line;
        out.append(f);
    };
    auto rankOf = [&](int i) { return g.nodes[i].rank; };
    auto nodeOf = [&](int i) { return g.nodes[i].node; };

    // ---- Structure ---------------------------------------------------------------------
    const CommPattern pat = detectPattern(g);
    {
        GraphFinding f;
        f.kind = QStringLiteral("pattern");
        f.category = QStringLiteral("structure");
        f.title = QStringLiteral("Pattern");
        switch (pat.kind) {
        case CommPattern::Kind::None:
            f.metric = QStringLiteral("collectives only");
            f.detail = QStringLiteral("no point-to-point messages in this window");
            break;
        case CommPattern::Kind::Ring:
            f.metric = QStringLiteral("ring");
            f.detail = QStringLiteral("every rank exchanges with its neighbours ±1");
            break;
        case CommPattern::Kind::Pipeline:
            f.metric = QStringLiteral("pipeline");
            f.detail = QStringLiteral("every rank sends to rank %1").arg(offsetsText(pat.offsets));
            break;
        case CommPattern::Kind::Stencil2D:
            f.metric = QStringLiteral("2D stencil %1 × %2").arg(pat.gridColumns).arg((n + pat.gridColumns - 1) / pat.gridColumns);
            f.detail = QStringLiteral("halo exchange with the partners at offsets %1 – each rank is a cell of a %2-wide grid and talks to its 4 grid neighbours")
                           .arg(offsetsText(pat.offsets)).arg(pat.gridColumns);
            break;
        case CommPattern::Kind::AllToAll:
            f.metric = QStringLiteral("all-to-all");
            f.detail = QStringLiteral("a rank talks to %1 of %2 other ranks").arg(pat.medianPartners).arg(n - 1);
            f.action = QStringLiteral("All-to-all P2P scales badly: consider MPI_Alltoall(v) or a hierarchical exchange");
            break;
        case CommPattern::Kind::Irregular:
            f.metric = QStringLiteral("irregular");
            f.detail = pat.offsets.isEmpty()
                ? QStringLiteral("a rank talks to %1 partners (median), no common offset – see the layout for the structure").arg(pat.medianPartners)
                : QStringLiteral("a rank talks to %1 partners (median), most at offsets %2").arg(pat.medianPartners).arg(offsetsText(pat.offsets));
            break;
        }
        finish(f, QStringLiteral("Pattern: %1 – %2").arg(f.metric, f.detail));
    }

    // Roles: hubs (far more messages than the others).
    {
        QVector<double> msgs(n, 0.0);
        double total = 0;
        for (const GraphEdge &e : g.edges)
            if (e.kind == GraphEdge::Kind::P2P && e.from < n && e.to < n) {
                msgs[e.from] += e.messages;
                msgs[e.to] += e.messages;
                total += 2.0 * e.messages;
            }
        if (total > 0 && n >= 6) {
            QVector<int> hubs;
            for (int i = 0; i < n; ++i)
                if (msgs[i] / total >= std::max(0.10, 4.0 / n))
                    hubs.append(i);
            std::sort(hubs.begin(), hubs.end(), [&](int a, int b) { return msgs[a] > msgs[b]; });
            if (!hubs.isEmpty() && hubs.size() <= 3) {
                GraphFinding f;
                f.kind = QStringLiteral("hub");
                f.category = QStringLiteral("structure");
                f.severity = msgs[hubs[0]] / total >= 0.3 ? 1 : 0;
                f.rank = rankOf(hubs[0]);
                QVector<int> rl;
                for (int h : std::as_const(hubs)) {
                    f.ranks.append(rankOf(h));
                    rl.append(rankOf(h));
                }
                f.value = msgs[hubs[0]] / total;
                f.title = hubs.size() == 1 ? QStringLiteral("Hub") : QStringLiteral("Hubs");
                f.metric = QStringLiteral("rank %1: %2 % of all messages").arg(rankList(rl)).arg(qRound(100 * f.value));
                f.detail = QStringLiteral("these ranks take part in a large share of all point-to-point messages (master / root role)");
                f.action = QStringLiteral("A central rank serialises the work: distribute it, or use a tree / collective instead of P2P to one rank");
                finish(f);
            }
        }
    }

    // Locality and a better rank -> node mapping.
    {
        double inside = 0, between = 0;
        for (const GraphEdge &e : g.edges) {
            if (e.kind != GraphEdge::Kind::P2P || e.from >= n || e.to >= n)
                continue;
            (g.nodes[e.from].group == g.nodes[e.to].group ? inside : between) += double(e.bytes);
        }
        if (g.groups.size() > 1 && inside + between > 0) {
            GraphFinding l;
            l.kind = QStringLiteral("locality");
            l.category = QStringLiteral("structure");
            l.value = inside / (inside + between);
            l.title = QStringLiteral("Locality");
            l.metric = QStringLiteral("%1 % inside nodes").arg(qRound(100 * l.value));
            l.detail = QStringLiteral("of the P2P volume (%1) stays inside the compute nodes, %2 crosses the network")
                           .arg(bytesText(inside + between)).arg(bytesText(between));
            finish(l, QStringLiteral("Locality: %1 % of the P2P volume (%2) stays inside the compute nodes, %3 crosses the network")
                          .arg(qRound(100 * l.value)).arg(bytesText(inside + between)).arg(bytesText(between)));
            const MappingSuggestion ms = suggestMapping(g, gridColumns);
            if (!ms.how.isEmpty() && ms.bestShare - ms.currentShare >= 0.10) {
                GraphFinding f;
                f.kind = QStringLiteral("mapping");
                f.category = QStringLiteral("structure");
                f.severity = 1;
                f.value = ms.bestShare;
                f.title = QStringLiteral("Better rank placement");
                f.metric = QStringLiteral("%1 % → %2 % inside nodes").arg(qRound(100 * ms.currentShare)).arg(qRound(100 * ms.bestShare));
                f.detail = QStringLiteral("with %1 more traffic would stay inside the compute nodes (found automatically)").arg(ms.how);
                f.action = QStringLiteral("Place the ranks accordingly: MPI_Cart_create / MPI_Dist_graph_create with reorder, or the rank order / --map-by of the job");
                finish(f);
            }
        }
    }

    // ---- Waiting -----------------------------------------------------------------------
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
        w.category = QStringLiteral("waiting");
        w.value = mean;
        w.title = QStringLiteral("Waiting");
        w.metric = QStringLiteral("%1 per rank").arg(msText(mean));
        w.detail = QStringLiteral("on average; %1 % of it in collectives (wait at collective), %2 % in P2P (late sender)")
                       .arg(qRound(100 * coll / sum)).arg(100 - qRound(100 * coll / sum));
        if (g.windowNs > 0 && mean / (g.windowNs / 1e9) > 0.2) {
            w.severity = 1;
            w.action = QStringLiteral("More than a fifth of the time is waiting: look at the load balance and the late senders below");
        }
        finish(w);
        QVector<int> order;
        for (int i = 0; i < n; ++i)
            if (g.nodes[i].kind == GraphNode::Kind::Rank && wait[i] > mean + 2 * sigma && wait[i] > 1.5 * mean)
                order.append(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return wait[a] > wait[b]; });
        if (!order.isEmpty()) {
            GraphFinding o;
            o.kind = QStringLiteral("outlier");
            o.category = QStringLiteral("waiting");
            o.rank = rankOf(order[0]);
            o.value = wait[order[0]];
            o.severity = 1;
            o.title = order.size() == 1 ? QStringLiteral("Waits far more") : QStringLiteral("%1 ranks wait far more").arg(order.size());
            QStringList names;
            for (int k = 0; k < order.size(); ++k) {
                o.ranks.append(rankOf(order[k]));
                if (k < 3)
                    names << QStringLiteral("%1 (%2)").arg(rankOf(order[k])).arg(nodeOf(order[k]));
            }
            if (order.size() > 3)
                names << QStringLiteral("…");
            o.metric = QStringLiteral("up to %1× the mean").arg(wait[order[0]] / mean, 0, 'f', 1);
            o.detail = QStringLiteral("rank %1 waits most (%2); they wait for slower partners")
                           .arg(names.join(QStringLiteral(", "))).arg(msText(wait[order[0]]));
            o.action = QStringLiteral("Follow the wait chain / late senders to the partner that is late");
            finish(o, QStringLiteral("Outlier: rank %1 (%2) waits %3 – %4× the mean")
                          .arg(o.rank).arg(nodeOf(order[0])).arg(msText(wait[order[0]])).arg(wait[order[0]] / mean, 0, 'f', 1));
        }
    }

    // Late senders (receivers wait for them), aggregated in one card.
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
        const double mean = causedSum / std::max(1, counted);
        for (int i = 0; i < n; ++i)
            if (caused[i] >= 2 * mean)
                order.append(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return caused[a] > caused[b]; });
        if (!order.isEmpty()) {
            GraphFinding c;
            c.kind = QStringLiteral("cause");
            c.category = QStringLiteral("waiting");
            c.rank = rankOf(order[0]);
            c.value = caused[order[0]];
            c.severity = 2;
            QVector<int> top;
            for (int k = 0; k < std::min<qsizetype>(3, order.size()); ++k)
                top.append(rankOf(order[k]));
            for (int k = 0; k < std::min<qsizetype>(3, order.size()); ++k) {
                c.ranks.append(rankOf(order[k]));
                for (const GraphEdge &e : g.edges)
                    if (e.kind == GraphEdge::Kind::P2P && e.from == order[k] && e.waitSeconds > 0 && e.to < n) {
                        c.ranks.append(rankOf(e.to));
                        c.links.append({rankOf(e.from), rankOf(e.to)});
                    }
            }
            c.title = order.size() == 1 ? QStringLiteral("Late sender") : QStringLiteral("Late senders");
            c.metric = QStringLiteral("rank %1: %2× the mean").arg(rankList(top, 3)).arg(caused[order[0]] / mean, 0, 'f', 1);
            c.detail = QStringLiteral("rank %1 (%2) makes its receivers wait %3 in total – likely slower than its neighbours%4")
                           .arg(c.rank).arg(nodeOf(order[0])).arg(msText(caused[order[0]]))
                           .arg(order.size() > 1 ? QStringLiteral("; %1 ranks stand out").arg(order.size()) : QString());
            c.action = QStringLiteral("Reduce the work of these ranks (load balance) or overlap: post MPI_Isend earlier, compute while messages travel");
            finish(c, QStringLiteral("Late sender: rank %1 (%2) makes its receivers wait %3 in total – %4× the mean, likely slower than its neighbours")
                          .arg(c.rank).arg(nodeOf(order[0])).arg(msText(caused[order[0]])).arg(caused[order[0]] / mean, 0, 'f', 1));
        }
    }

    // Late receivers: senders wait (blocking / synchronous sends) because the receive came late.
    {
        QVector<double> lr(n, 0.0); // by receiver: how long senders waited for it
        double lrSum = 0;
        for (const GraphEdge &e : g.edges)
            if (e.kind == GraphEdge::Kind::P2P && e.to < n && e.lateReceiverSeconds > 0) {
                lr[e.to] += e.lateReceiverSeconds;
                lrSum += e.lateReceiverSeconds;
            }
        if (lrSum > 0 && (sum <= 0 || lrSum >= 0.05 * sum)) {
            int worst = int(std::max_element(lr.cbegin(), lr.cend()) - lr.cbegin());
            GraphFinding f;
            f.kind = QStringLiteral("latereceiver");
            f.category = QStringLiteral("waiting");
            f.severity = 1;
            f.rank = rankOf(worst);
            f.value = lrSum;
            f.ranks.append(rankOf(worst));
            for (const GraphEdge &e : g.edges)
                if (e.kind == GraphEdge::Kind::P2P && e.to == worst && e.lateReceiverSeconds > 0 && e.from < n) {
                    f.ranks.append(rankOf(e.from));
                    f.links.append({rankOf(e.from), rankOf(e.to)});
                }
            f.title = QStringLiteral("Late receiver");
            f.metric = QStringLiteral("senders wait %1").arg(msText(lrSum));
            f.detail = QStringLiteral("senders are blocked until the receive is posted; most for rank %1 (%2, %3)")
                           .arg(f.rank).arg(nodeOf(worst)).arg(msText(lr[worst]));
            f.action = QStringLiteral("Post the receives earlier (MPI_Irecv before computing) or send non-blocking (MPI_Isend)");
            finish(f);
        }
    }

    // Wait chain: from the rank that waits most, follow whom it waited for, as long as that one
    // waited itself (a simplified critical path: who delays whom).
    {
        QVector<double> p2pWait(n, 0.0);
        for (const GraphEdge &e : g.edges)
            if (e.kind == GraphEdge::Kind::P2P && e.to < n)
                p2pWait[e.to] += e.waitSeconds;
        int cur = int(std::max_element(p2pWait.cbegin(), p2pWait.cend()) - p2pWait.cbegin());
        const double first = n > 0 ? p2pWait[cur] : 0;
        if (first > 0) {
            QVector<int> chain{cur};
            QSet<int> seen{cur};
            while (chain.size() < 8) {
                int bestFrom = -1;
                double bw = 0;
                for (const GraphEdge &e : g.edges)
                    if (e.kind == GraphEdge::Kind::P2P && e.to == cur && e.from < n && e.waitSeconds > bw) {
                        bw = e.waitSeconds;
                        bestFrom = e.from;
                    }
                if (bestFrom < 0 || seen.contains(bestFrom) || bw < 0.05 * first)
                    break;
                chain.append(bestFrom);
                seen.insert(bestFrom);
                if (p2pWait[bestFrom] < 0.1 * first)
                    break; // the origin: it does not wait itself
                cur = bestFrom;
            }
            if (chain.size() >= 3) {
                GraphFinding f;
                f.kind = QStringLiteral("chain");
                f.category = QStringLiteral("waiting");
                f.severity = 2;
                QStringList names;
                for (int k = int(chain.size()) - 1; k >= 0; --k)
                    names << QString::number(rankOf(chain[k]));
                for (int k = 0; k < chain.size(); ++k)
                    f.ranks.append(rankOf(chain[k]));
                for (int k = int(chain.size()) - 1; k > 0; --k)
                    f.links.append({rankOf(chain[k]), rankOf(chain[k - 1])});
                f.rank = rankOf(chain.last());
                f.value = first;
                f.title = QStringLiteral("Wait chain");
                f.metric = names.join(QStringLiteral(" → "));
                f.detail = QStringLiteral("delay passed on over %1 ranks: rank %2 is late, its receivers wait and are late themselves (rank %3 waits %4)")
                               .arg(chain.size()).arg(rankOf(chain.last())).arg(rankOf(chain.first())).arg(msText(first));
                f.action = QStringLiteral("Fix the origin (rank %1): its delay propagates through the whole chain").arg(rankOf(chain.last()));
                finish(f);
            }
        }
    }

    // Link with the most waiting.
    {
        int best = -1;
        for (int i = 0; i < g.edges.size(); ++i)
            if (g.edges[i].kind == GraphEdge::Kind::P2P && (best < 0 || g.edges[i].waitSeconds > g.edges[best].waitSeconds))
                best = i;
        if (best >= 0 && g.edges[best].waitSeconds > 0) {
            const GraphEdge &e = g.edges[best];
            GraphFinding l;
            l.kind = QStringLiteral("link");
            l.category = QStringLiteral("waiting");
            l.rank = rankOf(e.to);
            l.value = e.waitSeconds;
            l.severity = 1;
            l.fromRank = rankOf(e.from);
            l.toRank = rankOf(e.to);
            l.ranks = {l.fromRank, l.toRank};
            l.links = {{l.fromRank, l.toRank}};
            l.title = QStringLiteral("Most waiting on one link");
            l.metric = QStringLiteral("%1 → %2: %3").arg(l.fromRank).arg(l.toRank).arg(msText(e.waitSeconds));
            l.detail = QStringLiteral("rank %1 waits for rank %2's messages, %3 in %4 messages")
                           .arg(l.toRank).arg(l.fromRank).arg(msText(e.waitSeconds)).arg(e.messages);
            finish(l, QStringLiteral("Most waiting on one link: rank %1 → rank %2, %3 in %4 messages")
                          .arg(l.fromRank).arg(l.toRank).arg(msText(e.waitSeconds)).arg(e.messages));
        }
    }

    // ---- Load --------------------------------------------------------------------------
    // Compute time (window - time in MPI) per rank against the mean.
    if (g.windowNs > 0) {
        const double win = g.windowNs / 1e9;
        QVector<double> comp(n, 0.0);
        double csum = 0;
        int cn = 0;
        for (int i = 0; i < n; ++i) {
            if (g.nodes[i].kind != GraphNode::Kind::Rank)
                continue;
            comp[i] = std::max(0.0, win - g.nodes[i].mpiSeconds);
            csum += comp[i];
            ++cn;
        }
        const double cmean = cn > 0 ? csum / cn : 0;
        if (cmean > 0) {
            QVector<int> slow;
            for (int i = 0; i < n; ++i)
                if (g.nodes[i].kind == GraphNode::Kind::Rank && comp[i] >= 1.3 * cmean)
                    slow.append(i);
            std::sort(slow.begin(), slow.end(), [&](int a, int b) { return comp[a] > comp[b]; });
            if (!slow.isEmpty() && slow.size() <= std::max(1, cn / 3)) {
                GraphFinding f;
                f.kind = QStringLiteral("compute");
                f.category = QStringLiteral("load");
                f.severity = 2;
                f.rank = rankOf(slow[0]);
                f.value = comp[slow[0]] / cmean;
                QVector<int> rl;
                for (int i : std::as_const(slow)) {
                    f.ranks.append(rankOf(i));
                    rl.append(rankOf(i));
                }
                f.title = slow.size() == 1 ? QStringLiteral("Computes longer") : QStringLiteral("%1 ranks compute longer").arg(slow.size());
                f.metric = QStringLiteral("rank %1: +%2 %").arg(rankList(rl, 3)).arg(qRound(100 * (f.value - 1)));
                f.detail = QStringLiteral("rank %1 computes %2 (outside MPI) against %3 on average – the others wait for it")
                               .arg(f.rank).arg(msText(comp[slow[0]])).arg(msText(cmean));
                f.action = QStringLiteral("Balance the load: give these ranks less work (smaller sub-domain) or find what makes them slow");
                finish(f);
            }
        }
    }

    // Compute nodes whose ranks wait much less than the rest (likely the slow ones).
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
        for (int k = 0; k < groupWait.size(); ++k)
            if (groupRanks[k] > 0 && gmean - groupWait[k] > 1.5 * gsigma && groupWait[k] < 0.85 * gmean) {
                slow << g.groups[k];
                slowWait += groupWait[k];
            }
        if (!slow.isEmpty() && slow.size() <= gcount / 2) {
            GraphFinding s;
            s.kind = QStringLiteral("imbalance");
            s.category = QStringLiteral("load");
            s.value = slowWait / slow.size();
            s.severity = 2;
            s.nodes = slow;
            for (int i = 0; i < n; ++i)
                if (g.nodes[i].kind == GraphNode::Kind::Rank && slow.contains(g.nodes[i].node))
                    s.ranks.append(g.nodes[i].rank);
            QStringList shown = slow.mid(0, 8);
            if (slow.size() > 8)
                shown << QStringLiteral("…");
            s.title = QStringLiteral("Slow compute nodes");
            s.metric = QStringLiteral("%1 of %2 nodes").arg(slow.size()).arg(gcount);
            s.detail = QStringLiteral("the ranks of %1 wait only %2 on average (others %3) – the others wait for them, so these nodes are likely slower")
                           .arg(shown.join(QStringLiteral(", "))).arg(msText(s.value)).arg(msText(gmean));
            s.action = QStringLiteral("Check these nodes (CPU frequency, other jobs, hardware) or give them less work");
            finish(s, QStringLiteral("Imbalance: the ranks of %1 wait only %2 on average (others %3) – the others probably wait for them, these nodes are likely slower")
                          .arg(shown.join(QStringLiteral(", "))).arg(msText(s.value)).arg(msText(gmean)));
        }
    }

    // ---- Messages ----------------------------------------------------------------------
    {
        quint64 bytes = 0;
        qint64 msgs = 0;
        double smallMsgs = 0; // messages of edges whose average size is below 1 KiB
        for (const GraphEdge &e : g.edges) {
            if (e.kind != GraphEdge::Kind::P2P || e.messages <= 0)
                continue;
            bytes += e.bytes;
            msgs += e.messages;
            if (double(e.bytes) / e.messages < 1024)
                smallMsgs += e.messages;
        }
        if (msgs > 0) {
            GraphFinding f;
            f.kind = QStringLiteral("size");
            f.category = QStringLiteral("messages");
            const double avg = double(bytes) / double(msgs);
            const double smallShare = smallMsgs / double(msgs);
            const double perRank = double(msgs) / std::max(1, n);
            f.value = avg;
            f.title = QStringLiteral("Message sizes");
            f.metric = QStringLiteral("Ø %1, %2 messages").arg(bytesText(avg)).arg(msgs);
            f.detail = QStringLiteral("%1 % of the messages are below 1 KiB (latency-bound), %2 per rank in this window")
                           .arg(qRound(100 * smallShare)).arg(perRank, 0, 'f', perRank < 10 ? 1 : 0);
            if (smallShare >= 0.5 && perRank >= 20) {
                f.severity = 1;
                f.title = QStringLiteral("Many small messages");
                f.action = QStringLiteral("Bundle small messages (pack several into one, derived datatypes) – each message costs latency");
            } else if (avg >= 64 * 1024) {
                f.detail += QStringLiteral("; large messages: bandwidth-bound");
                f.action = QStringLiteral("Overlap large transfers with computation (non-blocking sends / receives)");
            }
            finish(f);
        }
    }
    return out;
}

QVector<GraphFinding> compareCommGraphs(const CommGraph &cur, const CommGraph &ref)
{
    QVector<GraphFinding> out;
    const int n = std::min(cur.rankNodeCount, ref.rankNodeCount);
    if (n == 0)
        return out;
    // By rank number (both rank level).
    QHash<int, int> ci, ri;
    for (int i = 0; i < cur.rankNodeCount; ++i)
        ci.insert(cur.nodes[i].rank, i);
    for (int i = 0; i < ref.rankNodeCount; ++i)
        ri.insert(ref.nodes[i].rank, i);
    const QVector<double> cw = nodeWaiting(cur), rw = nodeWaiting(ref);
    // Per second of window, so that windows of different length compare.
    const double cs = cur.windowNs > 0 ? cur.windowNs / 1e9 : 1, rs = ref.windowNs > 0 ? ref.windowNs / 1e9 : 1;
    double ctot = 0, rtot = 0;
    QVector<std::pair<double, int>> deltas;
    for (auto it = ci.cbegin(); it != ci.cend(); ++it) {
        const int r = it.key();
        const double c = cw[it.value()] / cs;
        const double p = ri.contains(r) ? rw[ri.value(r)] / rs : 0;
        ctot += c;
        rtot += p;
        deltas.append({c - p, r});
    }
    GraphFinding t;
    t.kind = QStringLiteral("compare");
    t.category = QStringLiteral("comparison");
    t.title = QStringLiteral("Waiting vs. reference");
    const double change = rtot > 0 ? (ctot - rtot) / rtot : 0;
    t.value = change;
    t.severity = change > 0.2 ? 2 : change > 0.05 ? 1 : 0;
    t.metric = rtot > 0 ? QStringLiteral("%1%2 %").arg(change >= 0 ? QStringLiteral("+") : QString()).arg(qRound(100 * change))
                        : QStringLiteral("no waiting in the reference");
    t.detail = QStringLiteral("waiting per second of the window, all ranks: %1 now, %2 in the reference")
                   .arg(msText(ctot)).arg(msText(rtot));
    t.text = t.title + QStringLiteral(": ") + t.metric;
    out.append(t);
    std::sort(deltas.begin(), deltas.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    if (!deltas.isEmpty() && deltas.first().first > 0) {
        GraphFinding f;
        f.kind = QStringLiteral("compare-ranks");
        f.category = QStringLiteral("comparison");
        f.severity = 1;
        QVector<int> rl;
        for (int k = 0; k < std::min<qsizetype>(5, deltas.size()) && deltas[k].first > 0; ++k) {
            f.ranks.append(deltas[k].second);
            rl.append(deltas[k].second);
        }
        f.rank = rl.first();
        f.title = QStringLiteral("Waits more than in the reference");
        f.metric = QStringLiteral("rank %1").arg(rankList(rl, 5));
        f.detail = QStringLiteral("rank %1 waits %2 per second more than in the reference window").arg(rl.first()).arg(msText(deltas.first().first));
        f.text = f.title + QStringLiteral(": ") + f.metric;
        out.append(f);
    }
    // Links whose waiting grew most.
    QHash<quint64, double> refLink;
    for (const GraphEdge &e : ref.edges)
        if (e.kind == GraphEdge::Kind::P2P && e.from < ref.rankNodeCount && e.to < ref.rankNodeCount)
            refLink[(quint64(quint32(ref.nodes[e.from].rank)) << 32) | quint32(ref.nodes[e.to].rank)] += e.waitSeconds / rs;
    double bestD = 0;
    int bf = -1, bt = -1;
    for (const GraphEdge &e : cur.edges) {
        if (e.kind != GraphEdge::Kind::P2P || e.from >= cur.rankNodeCount || e.to >= cur.rankNodeCount)
            continue;
        const int a = cur.nodes[e.from].rank, b = cur.nodes[e.to].rank;
        const double d = e.waitSeconds / cs - refLink.value((quint64(quint32(a)) << 32) | quint32(b));
        if (d > bestD) {
            bestD = d;
            bf = a;
            bt = b;
        }
    }
    if (bf >= 0) {
        GraphFinding f;
        f.kind = QStringLiteral("compare-link");
        f.category = QStringLiteral("comparison");
        f.severity = 1;
        f.fromRank = bf;
        f.toRank = bt;
        f.ranks = {bf, bt};
        f.links = {{bf, bt}};
        f.rank = bt;
        f.title = QStringLiteral("Link that got worse");
        f.metric = QStringLiteral("%1 → %2: +%3/s").arg(bf).arg(bt).arg(msText(bestD));
        f.detail = QStringLiteral("rank %1 waits for rank %2 %3 per second more than in the reference").arg(bt).arg(bf).arg(msText(bestD));
        f.text = f.title + QStringLiteral(": ") + f.metric;
        out.append(f);
    }
    return out;
}

namespace {

QVector<QPointF> basicLayout(const CommGraph &graph, GraphLayout layout, int gridColumns)
{
    QVector<QPointF> pos(graph.nodes.size());
    const int n = graph.rankNodeCount;
    const int comms = int(graph.nodes.size()) - n;

    // Process grid by rank number (all rank nodes, ranks 0 .. n - 1).
    bool byRank = layout == GraphLayout::Grid && gridColumns > 0;
    for (int i = 0; byRank && i < n; ++i)
        byRank = graph.nodes[i].kind == GraphNode::Kind::Rank && graph.nodes[i].rank >= 0 && graph.nodes[i].rank < n;
    if (byRank) {
        const int cols = gridColumns;
        const int rows = std::max(1, (n + cols - 1) / cols);
        const double yMax = comms > 0 ? 0.6 : 0.9;
        for (int i = 0; i < n; ++i) {
            const int r = graph.nodes[i].rank;
            const double x = cols > 1 ? -0.9 + 1.8 * (r % cols) / (cols - 1) : 0.0;
            const double y = rows > 1 ? -0.9 + (yMax + 0.9) * (r / cols) / (rows - 1) : 0.0;
            pos[i] = QPointF(x, y);
        }
        for (int k = 0; k < comms; ++k)
            pos[n + k] = QPointF(comms > 1 ? -0.9 + 1.8 * k / (comms - 1) : 0.0, 0.9);
        return pos;
    }

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


// ---------------------------------------------------------------------------------------
// Data-driven layouts.

// Undirected backbone of the P2P edges between rank / host nodes: per node its kBackbone
// heaviest edges (bytes, else messages), the union over all nodes. adj[i] = (neighbour, weight).
struct Backbone {
    int n = 0;
    QVector<QVector<std::pair<int, double>>> adj;
    int edges = 0;
};

Backbone backboneOf(const CommGraph &g)
{
    Backbone b;
    b.n = g.rankNodeCount;
    QHash<quint64, double> undirected;
    for (const GraphEdge &e : g.edges) {
        if (e.kind != GraphEdge::Kind::P2P || e.from >= b.n || e.to >= b.n || e.from == e.to)
            continue;
        const int a = std::min(e.from, e.to), c = std::max(e.from, e.to);
        undirected[(quint64(quint32(a)) << 32) | quint32(c)] += e.bytes > 0 ? double(e.bytes) : double(e.messages);
    }
    QVector<QVector<std::pair<int, double>>> all(b.n);
    for (auto it = undirected.cbegin(); it != undirected.cend(); ++it) {
        const int a = int(it.key() >> 32), c = int(it.key() & 0xffffffffu);
        all[a].append({c, it.value()});
        all[c].append({a, it.value()});
    }
    QSet<quint64> keep;
    for (int i = 0; i < b.n; ++i) {
        auto &l = all[i];
        std::sort(l.begin(), l.end(), [](const auto &x, const auto &y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
        for (int k = 0; k < std::min<qsizetype>(kBackbone, l.size()); ++k) {
            const int a = std::min(i, l[k].first), c = std::max(i, l[k].first);
            keep.insert((quint64(quint32(a)) << 32) | quint32(c));
        }
    }
    b.adj.resize(b.n);
    QList<quint64> sorted(keep.cbegin(), keep.cend());
    std::sort(sorted.begin(), sorted.end());
    for (quint64 key : std::as_const(sorted)) {
        const int a = int(key >> 32), c = int(key & 0xffffffffu);
        const double w = undirected.value(key);
        b.adj[a].append({c, w});
        b.adj[c].append({a, w});
        ++b.edges;
    }
    return b;
}

// Hop distances from src (unreachable: -1).
void bfs(const Backbone &b, int src, QVector<int> &dist, QVector<int> &queue)
{
    dist.fill(-1);
    queue.resize(0);
    dist[src] = 0;
    queue.append(src);
    for (qsizetype h = 0; h < queue.size(); ++h) {
        const int u = queue[h];
        for (const auto &[v, w] : b.adj[u])
            if (dist[v] < 0) {
                dist[v] = dist[u] + 1;
                queue.append(v);
            }
    }
}

// Positions centred and scaled into [-0.95, 0.95]² (aspect kept).
QVector<QPointF> normalised(QVector<QPointF> p)
{
    if (p.isEmpty())
        return p;
    double minX = p[0].x(), maxX = minX, minY = p[0].y(), maxY = minY;
    for (const QPointF &q : p) {
        minX = std::min(minX, q.x());
        maxX = std::max(maxX, q.x());
        minY = std::min(minY, q.y());
        maxY = std::max(maxY, q.y());
    }
    const double span = std::max({maxX - minX, maxY - minY, 1e-12});
    const QPointF c((minX + maxX) / 2, (minY + maxY) / 2);
    for (QPointF &q : p)
        q = (q - c) * (1.9 / span);
    return p;
}

// Pivot MDS (Brandes & Pich) on hop distances, refined by stress majorization (SMACOF with
// weights d^-2) over all pairs; previous positions (if given) as the start instead.
QVector<QPointF> stressLayout(const Backbone &b, const QVector<QPointF> *previous, const std::atomic_bool *cancel)
{
    const int n = b.n;
    QVector<QPointF> x(n);
    if (n <= 2) {
        for (int i = 0; i < n; ++i)
            x[i] = QPointF(i, 0);
        return normalised(x);
    }
    // All-pairs hop distances (n BFS on a sparse graph); unreachable = longest + 1.
    QVector<int> d(qsizetype(n) * n);
    QVector<int> dist(n), queue;
    int maxD = 1;
    for (int s = 0; s < n; ++s) {
        bfs(b, s, dist, queue);
        for (int t = 0; t < n; ++t) {
            d[qsizetype(s) * n + t] = dist[t];
            maxD = std::max(maxD, dist[t]);
        }
        if (cancel && cancel->load())
            return x;
    }
    for (int &v : d)
        if (v < 0)
            v = maxD + 1;

    const bool warm = previous && previous->size() == n;
    if (warm) {
        x = *previous;
    } else {
        // Pivot MDS: k pivots far apart, double-centred squared distances C (n x k), the two
        // largest eigenvectors of C^T C by power iteration, positions C v.
        const int k = std::min(n, 50);
        QVector<int> pivots{0};
        QVector<int> minDist(n);
        for (int t = 0; t < n; ++t)
            minDist[t] = d[t];
        while (pivots.size() < k) {
            int far = int(std::max_element(minDist.cbegin(), minDist.cend()) - minDist.cbegin());
            pivots.append(far);
            for (int t = 0; t < n; ++t)
                minDist[t] = std::min(minDist[t], d[qsizetype(far) * n + t]);
        }
        QVector<double> c(qsizetype(n) * k);
        QVector<double> colMean(k, 0.0), rowMean(n, 0.0);
        double all = 0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < k; ++j) {
                const double v = double(d[qsizetype(pivots[j]) * n + i]);
                c[qsizetype(i) * k + j] = v * v;
                colMean[j] += v * v / n;
                rowMean[i] += v * v / k;
                all += v * v / (double(n) * k);
            }
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < k; ++j)
                c[qsizetype(i) * k + j] = -0.5 * (c[qsizetype(i) * k + j] - rowMean[i] - colMean[j] + all);
        QVector<double> ctc(qsizetype(k) * k, 0.0);
        for (int a = 0; a < k; ++a)
            for (int bb = 0; bb < k; ++bb) {
                double sum = 0;
                for (int i = 0; i < n; ++i)
                    sum += c[qsizetype(i) * k + a] * c[qsizetype(i) * k + bb];
                ctc[qsizetype(a) * k + bb] = sum;
            }
        QVector<QVector<double>> vecs;
        for (int e = 0; e < 2; ++e) {
            QVector<double> v(k);
            for (int j = 0; j < k; ++j)
                v[j] = std::sin(1.0 + j * (e + 1) * 0.7) + 0.01 * j;
            for (int it = 0; it < 300; ++it) {
                QVector<double> nv(k, 0.0);
                for (int a = 0; a < k; ++a)
                    for (int bb = 0; bb < k; ++bb)
                        nv[a] += ctc[qsizetype(a) * k + bb] * v[bb];
                for (const auto &u : vecs) {
                    double dot = 0;
                    for (int j = 0; j < k; ++j)
                        dot += nv[j] * u[j];
                    for (int j = 0; j < k; ++j)
                        nv[j] -= dot * u[j];
                }
                double norm = 0;
                for (double q : nv)
                    norm += q * q;
                norm = std::sqrt(norm);
                if (norm < 1e-12)
                    break;
                for (int j = 0; j < k; ++j)
                    v[j] = nv[j] / norm;
            }
            vecs.append(v);
        }
        for (int i = 0; i < n; ++i) {
            double px = 0, py = 0;
            for (int j = 0; j < k; ++j) {
                px += c[qsizetype(i) * k + j] * vecs[0][j];
                py += c[qsizetype(i) * k + j] * vecs[1][j];
            }
            // Tiny deterministic jitter: nodes at equal pivot distances must not coincide.
            x[i] = QPointF(px + 1e-3 * std::sin(i * 12.9898), py + 1e-3 * std::cos(i * 78.233));
        }
    }
    // Scale to the graph distances, then SMACOF (Gansner et al., localised update).
    double num = 0, den = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const QPointF dd = x[i] - x[j];
            const double e = std::sqrt(dd.x() * dd.x() + dd.y() * dd.y());
            const double g = d[qsizetype(i) * n + j];
            num += e / g;
            den += e * e / (g * g);
        }
    const double scale = den > 0 ? num / den : 1;
    for (QPointF &q : x)
        q *= scale;
    const int iterations = warm ? 40 : 120;
    for (int it = 0; it < iterations; ++it) {
        if (cancel && cancel->load())
            return x;
        for (int i = 0; i < n; ++i) {
            double sx = 0, sy = 0, sw = 0;
            const int *row = d.constData() + qsizetype(i) * n;
            for (int j = 0; j < n; ++j) {
                if (j == i)
                    continue;
                const double g = row[j];
                const double w = 1.0 / (g * g);
                const QPointF dd = x[i] - x[j];
                const double e = std::max(1e-9, std::sqrt(dd.x() * dd.x() + dd.y() * dd.y()));
                sx += w * (x[j].x() + g * dd.x() / e);
                sy += w * (x[j].y() + g * dd.y() / e);
                sw += w;
            }
            x[i] = QPointF(sx / sw, sy / sw);
        }
    }
    return normalised(x);
}

// Two smallest non-trivial eigenvectors of the weighted Laplacian (power iteration on
// cI - L, deflating the constant vector).
QVector<QPointF> spectralCoordinates(const Backbone &b, const std::atomic_bool *cancel)
{
    const int n = b.n;
    QVector<QPointF> out(n);
    if (n < 3)
        return out;
    // Weights normalised (log scale: a few heavy edges must not dominate).
    QVector<QVector<std::pair<int, double>>> adj = b.adj;
    double maxDeg = 0;
    QVector<double> deg(n, 0.0);
    for (int i = 0; i < n; ++i) {
        for (auto &[j, w] : adj[i]) {
            w = 1.0 + std::log1p(w);
            deg[i] += w;
        }
        maxDeg = std::max(maxDeg, deg[i]);
    }
    const double c = 2 * maxDeg + 1e-9;
    QVector<QVector<double>> vecs;
    for (int e = 0; e < 2; ++e) {
        QVector<double> v(n);
        for (int i = 0; i < n; ++i)
            v[i] = std::sin(0.37 + i * (e + 1) * 1.618) + 1e-3 * i;
        const int iterations = std::clamp(n * 4, 300, 4000);
        for (int it = 0; it < iterations; ++it) {
            if (cancel && (it & 63) == 0 && cancel->load())
                return out;
            QVector<double> nv(n);
            for (int i = 0; i < n; ++i) {
                double lv = deg[i] * v[i];
                for (const auto &[j, w] : adj[i])
                    lv -= w * v[j];
                nv[i] = c * v[i] - lv;
            }
            double mean = 0;
            for (double q : nv)
                mean += q / n;
            for (double &q : nv)
                q -= mean;
            for (const auto &u : vecs) {
                double dot = 0;
                for (int i = 0; i < n; ++i)
                    dot += nv[i] * u[i];
                for (int i = 0; i < n; ++i)
                    nv[i] -= dot * u[i];
            }
            double norm = 0;
            for (double q : nv)
                norm += q * q;
            norm = std::sqrt(norm);
            if (norm < 1e-15)
                break;
            for (int i = 0; i < n; ++i)
                v[i] = nv[i] / norm;
        }
        vecs.append(v);
    }
    for (int i = 0; i < n; ++i)
        out[i] = QPointF(vecs[0][i], vecs[1][i]);
    return out;
}

// Eigen-decomposition of a small symmetric matrix (Jacobi rotations): eigenvalues ascending,
// eigenvectors as columns of vecs (row-major dim x dim).
void jacobiEigen(QVector<double> a, int dim, QVector<double> &values, QVector<double> &vecs)
{
    vecs = QVector<double>(dim * dim, 0.0);
    for (int i = 0; i < dim; ++i)
        vecs[i * dim + i] = 1;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0;
        for (int p = 0; p < dim; ++p)
            for (int q = p + 1; q < dim; ++q)
                off += a[p * dim + q] * a[p * dim + q];
        if (off < 1e-22)
            break;
        for (int p = 0; p < dim; ++p)
            for (int q = p + 1; q < dim; ++q) {
                const double apq = a[p * dim + q];
                if (std::abs(apq) < 1e-300)
                    continue;
                const double theta = (a[q * dim + q] - a[p * dim + p]) / (2 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < dim; ++k) {
                    const double akp = a[k * dim + p], akq = a[k * dim + q];
                    a[k * dim + p] = c * akp - s * akq;
                    a[k * dim + q] = s * akp + c * akq;
                }
                for (int k = 0; k < dim; ++k) {
                    const double apk = a[p * dim + k], aqk = a[q * dim + k];
                    a[p * dim + k] = c * apk - s * aqk;
                    a[q * dim + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < dim; ++k) {
                    const double vkp = vecs[k * dim + p], vkq = vecs[k * dim + q];
                    vecs[k * dim + p] = c * vkp - s * vkq;
                    vecs[k * dim + q] = s * vkp + c * vkq;
                }
            }
    }
    // Sort ascending.
    QVector<int> order(dim);
    for (int i = 0; i < dim; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](int x, int y) { return a[x * dim + x] < a[y * dim + y]; });
    values.resize(dim);
    QVector<double> sorted(dim * dim);
    for (int j = 0; j < dim; ++j) {
        values[j] = a[order[j] * dim + order[j]];
        for (int k = 0; k < dim; ++k)
            sorted[k * dim + j] = vecs[k * dim + order[j]];
    }
    vecs = sorted;
}

// The k smallest non-trivial eigenvectors of the (log-weighted) Laplacian by block power
// iteration on cI - L with orthonormalisation (constant vector deflated).
QVector<QVector<double>> laplacianEigenvectors(const Backbone &b, int k, const std::atomic_bool *cancel)
{
    const int n = b.n;
    QVector<QVector<std::pair<int, double>>> adj = b.adj;
    QVector<double> deg(n, 0.0);
    double maxDeg = 0;
    for (int i = 0; i < n; ++i) {
        for (auto &[j, w] : adj[i]) {
            w = 1.0 + std::log1p(w) * 0; // structure only: equal weights
            deg[i] += w;
        }
        maxDeg = std::max(maxDeg, deg[i]);
    }
    const double c = 2 * maxDeg + 1e-9;
    QVector<QVector<double>> v(k, QVector<double>(n));
    for (int e = 0; e < k; ++e)
        for (int i = 0; i < n; ++i)
            v[e][i] = std::sin(0.37 + i * (e + 1) * 1.618 + e) + 1e-3 * ((i * 7 + e) % 11);
    auto orthonormalise = [&] {
        for (int e = 0; e < k; ++e) {
            double mean = 0;
            for (double q : v[e])
                mean += q / n;
            for (double &q : v[e])
                q -= mean;
            for (int f = 0; f < e; ++f) {
                double dot = 0;
                for (int i = 0; i < n; ++i)
                    dot += v[e][i] * v[f][i];
                for (int i = 0; i < n; ++i)
                    v[e][i] -= dot * v[f][i];
            }
            double norm = 0;
            for (double q : v[e])
                norm += q * q;
            norm = std::sqrt(std::max(norm, 1e-300));
            for (double &q : v[e])
                q /= norm;
        }
    };
    orthonormalise();
    const int iterations = std::clamp(n * 3, 400, 3000);
    QVector<double> nv(n);
    for (int it = 0; it < iterations; ++it) {
        if (cancel && (it & 63) == 0 && cancel->load())
            break;
        for (int e = 0; e < k; ++e) {
            for (int i = 0; i < n; ++i) {
                double lv = deg[i] * v[e][i];
                for (const auto &[j, w] : adj[i])
                    lv -= w * v[e][j];
                nv[i] = c * v[e][i] - lv;
            }
            v[e] = nv;
        }
        orthonormalise();
    }
    return v;
}

// Splits a 4-dimensional (degenerate) eigenspace of a square torus into its two circle planes:
// the quadratic forms Q with u_i^T Q u_i equal for all nodes are the plane projections; one
// orthogonal to the identity, eigen-decomposed, separates them. Returns 4 vectors (plane 1, 2).
QVector<QVector<double>> splitPlanes(const QVector<QVector<double>> &v, int n)
{
    constexpr int kD = 10;
    const int pairs[kD][2] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
    QVector<double> mean(kD, 0.0);
    QVector<QVector<double>> f(n, QVector<double>(kD));
    for (int i = 0; i < n; ++i)
        for (int d = 0; d < kD; ++d) {
            const double x = v[pairs[d][0]][i] * v[pairs[d][1]][i] * n * (pairs[d][0] == pairs[d][1] ? 1 : 2);
            f[i][d] = x;
            mean[d] += x / n;
        }
    QVector<double> ata(kD * kD, 0.0);
    for (int i = 0; i < n; ++i)
        for (int x = 0; x < kD; ++x)
            for (int y = 0; y < kD; ++y)
                ata[x * kD + y] += (f[i][x] - mean[x]) * (f[i][y] - mean[y]);
    QVector<double> id(kD, 0.0);
    for (int d = 0; d < 4; ++d)
        id[d] = 0.5;
    double trace = 0;
    for (int x = 0; x < kD; ++x)
        trace += ata[x * kD + x];
    for (int x = 0; x < kD; ++x)
        for (int y = 0; y < kD; ++y)
            ata[x * kD + y] += (trace + 1) * id[x] * id[y]; // the identity is not wanted
    QVector<double> vals, vecs;
    jacobiEigen(ata, kD, vals, vecs);
    QVector<double> q(16, 0.0);
    for (int d = 0; d < kD; ++d) {
        const double x = vecs[d * kD + 0];
        const int r = pairs[d][0], c = pairs[d][1];
        q[r * 4 + c] += x;
        if (r != c)
            q[c * 4 + r] += x;
    }
    QVector<double> qv, qe;
    jacobiEigen(q, 4, qv, qe);
    QVector<QVector<double>> out(4, QVector<double>(n, 0.0));
    const int order[4] = {3, 2, 1, 0}; // plane 1: two largest, plane 2: two smallest
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < n; ++i)
            for (int d = 0; d < 4; ++d)
                out[k][i] += v[d][i] * qe[d * 4 + order[k]];
    return out;
}

// Angle of each node in the plane of two vectors, and how circular the points are
// (coefficient of variation of the radius; 0 = a perfect circle).
QVector<double> anglesOf(const QVector<double> &x, const QVector<double> &y, double *spread)
{
    const int n = int(x.size());
    QVector<double> ang(n);
    double sum = 0, sum2 = 0;
    for (int i = 0; i < n; ++i) {
        ang[i] = std::atan2(y[i], x[i]);
        const double r = std::hypot(x[i], y[i]);
        sum += r;
        sum2 += r * r;
    }
    const double mean = sum / std::max(1, n);
    *spread = mean > 0 ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) / mean : 1;
    return ang;
}

// |mean of exp(i (a - m b))|: 1 when angle a is m times angle b (a harmonic), 0 when unrelated.
double harmonicOf(const QVector<double> &a, const QVector<double> &b, int m)
{
    double c = 0, s = 0;
    for (int i = 0; i < a.size(); ++i) {
        c += std::cos(a[i] - m * b[i]);
        s += std::sin(a[i] - m * b[i]);
    }
    return std::hypot(c, s) / std::max<qsizetype>(1, a.size());
}

// Torus coordinates without knowing the numbering: the smallest non-trivial Laplacian
// eigenvectors (Rayleigh-Ritz on 8 power-iterated vectors) come in pairs of equal eigenvalue;
// each pair of a torus direction puts the nodes on a circle. The first circle pair and the next
// one that is not a harmonic of it (2x, 3x the first angle) give the two angles. A degenerate
// group of four (square torus) is split by splitPlanes. Empty if no two circles are found.
QVector<QPointF> torusCoordinates(const Backbone &b, const std::atomic_bool *cancel)
{
    const int n = b.n;
    if (n < 9)
        return {};
    constexpr int kK = 8;
    QVector<QVector<double>> v = laplacianEigenvectors(b, kK, cancel);
    // Rayleigh-Ritz: eigenpairs within the subspace.
    auto lap = [&](const QVector<double> &x) {
        QVector<double> y(n);
        for (int i = 0; i < n; ++i) {
            double d = 0, s = 0;
            for (const auto &[j, w] : b.adj[i]) {
                d += 1;
                s += x[j];
            }
            y[i] = d * x[i] - s;
        }
        return y;
    };
    QVector<QVector<double>> lv(kK);
    for (int e = 0; e < kK; ++e)
        lv[e] = lap(v[e]);
    QVector<double> m(kK * kK);
    for (int x = 0; x < kK; ++x)
        for (int y = 0; y < kK; ++y) {
            double dot = 0;
            for (int i = 0; i < n; ++i)
                dot += v[x][i] * lv[y][i];
            m[x * kK + y] = dot;
        }
    for (int x = 0; x < kK; ++x)
        for (int y = x + 1; y < kK; ++y)
            m[x * kK + y] = m[y * kK + x] = 0.5 * (m[x * kK + y] + m[y * kK + x]);
    QVector<double> vals, vecs;
    jacobiEigen(m, kK, vals, vecs);
    QVector<QVector<double>> u(kK, QVector<double>(n, 0.0));
    for (int e = 0; e < kK; ++e)
        for (int i = 0; i < n; ++i)
            for (int d = 0; d < kK; ++d)
                u[e][i] += v[d][i] * vecs[d * kK + e];

    // Groups of (nearly) equal eigenvalues; pairs are circle candidates.
    QVector<QPair<QVector<double>, QVector<double>>> circles;
    for (int e = 0; e < kK;) {
        int f = e + 1;
        while (f < kK && std::abs(vals[f] - vals[e]) <= 0.03 * std::max(1e-9, std::abs(vals[e])))
            ++f;
        const int size = f - e;
        if (size == 2) {
            circles.append({u[e], u[e + 1]});
        } else if (size == 4) {
            const QVector<QVector<double>> p = splitPlanes({u[e], u[e + 1], u[e + 2], u[e + 3]}, n);
            circles.append({p[0], p[1]});
            circles.append({p[2], p[3]});
        }
        e = f;
    }
    if (circles.size() < 2)
        return {};
    double spread1 = 1;
    const QVector<double> a1 = anglesOf(circles[0].first, circles[0].second, &spread1);
    if (spread1 > 0.35)
        return {};
    for (int c = 1; c < circles.size(); ++c) {
        double spread2 = 1;
        const QVector<double> a2 = anglesOf(circles[c].first, circles[c].second, &spread2);
        if (spread2 > 0.35)
            continue;
        bool harmonic = false;
        for (int mult = 2; mult <= 4 && !harmonic; ++mult)
            harmonic = harmonicOf(a2, a1, mult) > 0.7 || harmonicOf(a2, a1, -mult) > 0.7;
        if (harmonic || harmonicOf(a2, a1, 1) > 0.7 || harmonicOf(a2, a1, -1) > 0.7)
            continue;
        QVector<QPointF> out(n);
        for (int i = 0; i < n; ++i)
            out[i] = QPointF(a1[i] / kPi * 0.95, a2[i] / kPi * 0.95);
        return out;
    }
    return {};
}

QVector<QPointF> circleInOrder(const QVector<int> &order, int n)
{
    QVector<QPointF> p(n);
    for (int k = 0; k < order.size(); ++k) {
        const double a = -kPi / 2 + 2 * kPi * k / std::max<qsizetype>(1, order.size());
        p[order[k]] = QPointF(std::cos(a), std::sin(a));
    }
    return p;
}

// Rank / host positions of a data-driven layout plus communicators: one in the middle, several
// on a small circle around the centre of the picture.
QVector<QPointF> withCommunicators(const CommGraph &g, QVector<QPointF> rankPos)
{
    const int n = g.rankNodeCount;
    const int comms = int(g.nodes.size()) - n;
    rankPos.resize(g.nodes.size());
    for (int k = 0; k < comms; ++k)
        rankPos[n + k] = comms == 1 ? QPointF(0, 0)
                                     : QPointF(0.15 * std::cos(2 * kPi * k / comms), 0.15 * std::sin(2 * kPi * k / comms));
    return rankPos;
}

QVector<QPointF> dataLayout(const CommGraph &g, const Backbone &b, GraphLayout layout, const QVector<QPointF> *previous,
                            const std::atomic_bool *cancel)
{
    const int n = g.rankNodeCount;
    if (b.edges == 0)
        return basicLayout(g, GraphLayout::Circle, 0);
    switch (layout) {
    case GraphLayout::Stress: {
        QVector<QPointF> prev;
        if (previous && previous->size() >= n)
            prev = previous->mid(0, n);
        return withCommunicators(g, stressLayout(b, prev.isEmpty() ? nullptr : &prev, cancel));
    }
    case GraphLayout::Spectral:
        return withCommunicators(g, normalised(spectralCoordinates(b, cancel)));
    case GraphLayout::Torus: {
        // Communicators below the picture (the torus fills the square). No torus found: the
        // circle (scores low against the others).
        QVector<QPointF> p = torusCoordinates(b, cancel);
        if (p.size() != n)
            return basicLayout(g, GraphLayout::Circle, 0);
        for (QPointF &x : p)
            x = QPointF(x.x(), x.y() * 0.8 - 0.1);
        const int comms = int(g.nodes.size()) - n;
        p.resize(g.nodes.size());
        for (int k = 0; k < comms; ++k)
            p[n + k] = QPointF(comms > 1 ? -0.9 + 1.8 * k / (comms - 1) : 0.0, 0.92);
        return p;
    }
    case GraphLayout::SpectralCircle: {
        const QVector<QPointF> sc = spectralCoordinates(b, cancel);
        QVector<int> order(n);
        for (int i = 0; i < n; ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int c) {
            return std::atan2(sc[a].y(), sc[a].x()) < std::atan2(sc[c].y(), sc[c].x());
        });
        QVector<QPointF> p = circleInOrder(order, n);
        // Communicators as in the circle layout.
        const QVector<QPointF> basic = basicLayout(g, GraphLayout::Circle, 0);
        p.resize(g.nodes.size());
        for (int i = n; i < g.nodes.size(); ++i)
            p[i] = basic[i];
        return p;
    }
    default:
        return basicLayout(g, layout, 0);
    }
}

double preservation(const Backbone &b, const QVector<QPointF> &pos)
{
    const int n = b.n;
    if (n < 2 || b.edges == 0 || pos.size() < n)
        return 0;
    double sum = 0;
    int counted = 0;
    QVector<std::pair<double, int>> near(n);
    for (int i = 0; i < n; ++i) {
        const int k = int(b.adj[i].size());
        if (k == 0)
            continue;
        for (int j = 0; j < n; ++j) {
            const QPointF dd = pos[i] - pos[j];
            near[j] = {j == i ? 1e300 : dd.x() * dd.x() + dd.y() * dd.y(), j};
        }
        std::nth_element(near.begin(), near.begin() + std::min(k, n - 1), near.end());
        // The k nearest (ties at the k-th distance count as near too).
        const double kth = near[std::min(k, n - 1)].first;
        int hit = 0;
        for (const auto &[v, w] : b.adj[i]) {
            const QPointF dd = pos[i] - pos[v];
            if (dd.x() * dd.x() + dd.y() * dd.y() <= kth * (1 + 1e-9))
                ++hit;
        }
        sum += double(std::min(hit, k)) / k;
        ++counted;
    }
    if (counted == 0)
        return 0;
    // Penalty for nodes on top of each other (a collapsed layout keeps neighbours "near" but
    // shows nothing): the share of nodes closer to another one than a quarter of the spacing of
    // n nodes spread evenly over the picture.
    const double minSep = 0.25 * 1.9 / std::sqrt(double(n));
    int crowded = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            if (j == i)
                continue;
            const QPointF dd = pos[i] - pos[j];
            if (dd.x() * dd.x() + dd.y() * dd.y() < minSep * minSep) {
                ++crowded;
                break;
            }
        }
    return sum / counted * (1.0 - double(crowded) / n);
}

// All-pairs hop distances of the backbone (unreachable: longest + 1).
QVector<int> hopDistances(const Backbone &b)
{
    const int n = b.n;
    QVector<int> d(qsizetype(n) * n);
    QVector<int> dist(n), queue;
    int maxD = 1;
    for (int s = 0; s < n; ++s) {
        bfs(b, s, dist, queue);
        for (int t = 0; t < n; ++t) {
            d[qsizetype(s) * n + t] = dist[t];
            maxD = std::max(maxD, dist[t]);
        }
    }
    for (int &v : d)
        if (v < 0)
            v = maxD + 1;
    return d;
}

// Normalised stress (0 = picture distances proportional to hop distances, 1 = unrelated),
// with the best scale, weights d^-2.
double normalisedStress(const QVector<int> &d, int n, const QVector<QPointF> &pos)
{
    double se = 0, ee = 0, dd = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double g = d[qsizetype(i) * n + j];
            const QPointF v = pos[i] - pos[j];
            const double e = std::sqrt(v.x() * v.x() + v.y() * v.y());
            const double w = 1.0 / (g * g);
            se += w * g * e;
            ee += w * e * e;
            dd += w * g * g;
        }
    if (ee <= 0 || dd <= 0)
        return 1;
    const double s = se / ee; // best scale
    double st = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double g = d[qsizetype(i) * n + j];
            const QPointF v = pos[i] - pos[j];
            const double e = std::sqrt(v.x() * v.x() + v.y() * v.y());
            st += (g - s * e) * (g - s * e) / (g * g);
        }
    return std::clamp(st / dd, 0.0, 1.0);
}

} // namespace

QString layoutName(GraphLayout layout)
{
    switch (layout) {
    case GraphLayout::Circle: return QStringLiteral("circle");
    case GraphLayout::Grid: return QStringLiteral("grid");
    case GraphLayout::Stress: return QStringLiteral("stress");
    case GraphLayout::Spectral: return QStringLiteral("spectral");
    case GraphLayout::SpectralCircle: return QStringLiteral("spectral circle");
    case GraphLayout::Torus: return QStringLiteral("torus");
    }
    return {};
}

QVector<QPointF> layoutGraph(const CommGraph &graph, GraphLayout layout, int gridColumns, const QVector<QPointF> *previous)
{
    if (layout == GraphLayout::Circle || layout == GraphLayout::Grid)
        return basicLayout(graph, layout, gridColumns);
    return dataLayout(graph, backboneOf(graph), layout, previous, nullptr);
}

quint64 backboneSignature(const CommGraph &graph)
{
    const Backbone b = backboneOf(graph);
    quint64 h = 1469598103934665603ull ^ quint64(b.n);
    for (int i = 0; i < b.n; ++i)
        for (const auto &[j, w] : b.adj[i])
            if (j > i)
                h = (h ^ ((quint64(quint32(i)) << 32) | quint32(j))) * 1099511628211ull;
    return h;
}

double neighbourhoodPreservation(const CommGraph &graph, const QVector<QPointF> &positions)
{
    return preservation(backboneOf(graph), positions);
}

LayoutChoice autoLayout(const CommGraph &graph, int gridColumns, const QVector<QPointF> *previous,
                        const std::atomic_bool *cancel)
{
    LayoutChoice best;
    const Backbone b = backboneOf(graph);
    best.signature = backboneSignature(graph);
    best.positions = basicLayout(graph, GraphLayout::Circle, 0);
    if (b.edges == 0)
        return best;
    // Stress first: the general method; the others must be clearly better.
    QVector<GraphLayout> candidates{GraphLayout::Stress};
    if (gridColumns > 0)
        candidates.append(GraphLayout::Grid);
    // The torus candidate only for graphs shaped like a 2D torus: (almost) every node with
    // 4 neighbours in the backbone.
    int four = 0;
    for (int i = 0; i < b.n; ++i)
        four += b.adj[i].size() == 4 ? 1 : 0;
    const bool regular4 = b.n >= 9 && four >= 0.9 * b.n;
    if (regular4)
        candidates << GraphLayout::Torus;
    candidates << GraphLayout::Circle << GraphLayout::SpectralCircle << GraphLayout::Spectral;
    // Each candidate: neighbourhood preservation (partners next to each other, crowding
    // penalised) and distance faithfulness (1 - normalised stress against the hop distances).
    // Eligible are the candidates with at least 40 % of the best preservation (a folded torus or
    // a collapsed spectral picture drops out; only when some candidate preserves at least 30 %);
    // among them the most faithful wins, a later one only when better by 0.02.
    const QVector<int> hops = hopDistances(b);
    struct Cand {
        GraphLayout layout;
        QVector<QPointF> pos;
        double np;
        double faithful;
    };
    QVector<Cand> cands;
    double maxNp = 0;
    for (GraphLayout l : std::as_const(candidates)) {
        if (cancel && cancel->load())
            break;
        Cand c{l, l == GraphLayout::Grid ? basicLayout(graph, l, gridColumns) : dataLayout(graph, b, l, previous, cancel), 0, 0};
        c.np = preservation(b, c.pos);
        c.faithful = 1.0 - normalisedStress(hops, b.n, c.pos);
        maxNp = std::max(maxNp, c.np);
        if (qEnvironmentVariableIsSet("EDUMPI_LAYOUT_DEBUG"))
            qInfo("layout %s: np %.3f faithful %.3f", qPrintable(layoutName(l)), c.np, c.faithful);
        cands.append(std::move(c));
    }
    best.score = -1;
    // A torus cut open clearly beats a folded stress picture: taken when its neighbourhood
    // preservation is high and far above the stress layout's (a periodic grid; a detected
    // process grid that is as good stays).
    {
        const Cand *torus = nullptr, *stress = nullptr, *grid = nullptr;
        for (const Cand &c : std::as_const(cands)) {
            if (c.layout == GraphLayout::Torus)
                torus = &c;
            else if (c.layout == GraphLayout::Stress)
                stress = &c;
            else if (c.layout == GraphLayout::Grid)
                grid = &c;
        }
        if (regular4 && torus && torus->np >= 0.85 && (!stress || torus->np >= stress->np + 0.25)) {
            const Cand &pick = grid && grid->np >= torus->np - 0.01 ? *grid : *torus;
            for (const Cand &c : std::as_const(cands))
                best.scores.append({c.layout, c.faithful});
            best.layout = pick.layout;
            best.positions = pick.pos;
            best.score = pick.faithful;
            return best;
        }
    }
    for (const Cand &c : std::as_const(cands)) {
        best.scores.append({c.layout, c.faithful});
        if (maxNp >= 0.3 && c.np + 1e-9 < 0.4 * maxNp)
            continue;
        if (c.faithful > best.score + 0.02) {
            best.score = c.faithful;
            best.layout = c.layout;
            best.positions = c.pos;
        }
    }
    return best;
}

} // namespace trace
