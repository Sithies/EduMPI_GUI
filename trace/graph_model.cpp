#include "graph_model.h"

#include <QHash>

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
        if (nodesLevel) {
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
                    if (nodesLevel) {
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
