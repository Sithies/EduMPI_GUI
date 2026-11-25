#pragma once

#include <QPointF>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "timeline_store.h"

// Communication graph of a trace window (Trace Graph view): ranks (or nodes) as graph nodes,
// P2P messages as edges between them and collectives as star edges to one node per
// communicator. Pure logic without Qt Quick, used by TraceGraphItem. All times are nanoseconds
// relative to trace start.
//
// Which records count for a window [fromNs, toNs) (the rules of TimelineStore::aggregate, so the
// numbers match "Show window in 3D/2D/Matrix"):
// - a P2P message counts when the call that sent it starts in the window (matched messages only;
//   unmatched sends and messages to self are left out);
// - a collective participation counts when the rank's collective call starts in the window.

namespace trace {

enum class GraphWeight { Bytes, Messages, Wait };
enum class GraphLevel { Ranks, Nodes };
enum class GraphLayout { Circle, Grid };

struct GraphOptions {
    bool p2p = true;
    bool collectives = true;
    bool directed = true; // false: a -> b and b -> a are merged into one edge
    GraphLevel level = GraphLevel::Ranks;
};

struct GraphNode {
    enum class Kind : quint8 { Rank, Node, Communicator };
    Kind kind = Kind::Rank;
    int rank = -1;       // Kind::Rank
    QString node;        // Kind::Rank: the rank's node; Kind::Node: the node itself
    quint64 comm = 0;    // Kind::Communicator
    int members = 0;     // Kind::Communicator: size; Kind::Node: number of ranks
    int group = 0;       // index of the node (host) the graph node belongs to; -1 for communicators
    // Kind::Node only: traffic between ranks of this node (no edge is drawn for it).
    quint64 internalBytes = 0;
    int internalMessages = 0;
};

struct GraphEdge {
    enum class Kind : quint8 { P2P, Collective };
    Kind kind = Kind::P2P;
    int from = -1; // indices into CommGraph::nodes; collectives: rank/node -> communicator
    int to = -1;
    quint64 bytes = 0;
    int messages = 0;   // P2P: messages; collective: calls
    double waitSeconds = 0; // P2P: late sender of the receiver; collective: late sender of the rank
};

struct CommGraph {
    QVector<GraphNode> nodes; // ranks (or nodes) first, in overview row order; communicators last
    QVector<GraphEdge> edges;
    QStringList groups;       // node (host) names, index = GraphNode::group
    int rankNodeCount = 0;    // nodes before the communicators

    int indexOfRank(int rank) const; // Ranks level: graph node of that rank, else -1
};

// Graph of [fromNs, toNs) from a store holding the activities of that window (complete or
// covering it); empty edges otherwise. Graph nodes are all ranks of the store (also those
// without traffic, so the layout stays stable while the window moves) resp. all nodes (hosts).
CommGraph buildCommGraph(const TimelineStore &store, qint64 fromNs, qint64 toNs, const GraphOptions &options);

// Communicator id of the node that mergeCommunicators() makes.
constexpr quint64 kMergedCommunicators = ~quint64(0);

// The graph with all communicator nodes merged into one (comm = kMergedCommunicators, members =
// number of merged communicators); collective edges of a rank / host are summed. For traces
// with many communicators, whose nodes would cover each other in the middle.
CommGraph mergeCommunicators(const CommGraph &graph);

double edgeWeight(const GraphEdge &edge, GraphWeight weight);

// Edges to draw, heaviest first (equal weights spread over the graph, not by node order): at
// most maxEdges (0 = all) by weight, plus every edge of
// focusNode (-1 = none) even beyond that limit. Edges with weight 0 are left out except under
// GraphWeight::Wait, where messages without waiting still show the communication.
QVector<int> selectEdges(const CommGraph &graph, GraphWeight weight, int maxEdges, int focusNode);

// Positions in the unit square [-1, 1]² (y down), one per graph node.
// Circle: rank/host nodes on a circle in graph order (ranks grouped by node, as in the
// overview), communicators on a small inner circle (one at the centre).
// Grid: rank/host nodes row by row in a near-square grid, communicators in a row below it.
QVector<QPointF> layoutGraph(const CommGraph &graph, GraphLayout layout);

} // namespace trace
