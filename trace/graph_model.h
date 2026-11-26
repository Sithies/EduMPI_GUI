#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

#include <atomic>
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
// Circle: graph order (ranks grouped by compute node). Grid: near-square grid, or the process
// grid of a detected stencil (gridColumns). Data-driven (from the P2P edges, see autoLayout):
// Stress: distances in the picture follow the hop distances in the graph (pivot MDS +
// stress majorization); Spectral: the two smallest non-trivial Laplacian eigenvectors;
// SpectralCircle: the circle in the order of the spectral angle (neighbours next to each other).
enum class GraphLayout { Circle, Grid, Stress, Spectral, SpectralCircle };

struct GraphOptions {
    bool p2p = true;
    bool collectives = true;
    bool directed = true; // false: a -> b and b -> a are merged into one edge
    GraphLevel level = GraphLevel::Ranks;
    // Nodes level: hosts shown as their single ranks (opened by the user), the rest merged.
    QStringList expandedNodes;
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
    // Rank / host: time in MPI calls within the window (seconds, summed over a host's ranks)
    // and the ranks it stands for (1 for a rank).
    double mpiSeconds = 0;
    int rankCount = 1;
};

struct GraphEdge {
    enum class Kind : quint8 { P2P, Collective };
    Kind kind = Kind::P2P;
    int from = -1; // indices into CommGraph::nodes; collectives: rank/node -> communicator
    int to = -1;
    quint64 bytes = 0;
    int messages = 0;   // P2P: messages; collective: calls
    double waitSeconds = 0; // P2P: late sender of the receiver; collective: late sender of the rank
    double lateReceiverSeconds = 0; // P2P: the sender's waiting for a late receive (blocking sends)
};

struct CommGraph {
    QVector<GraphNode> nodes; // ranks (or nodes) first, in overview row order; communicators last
    QVector<GraphEdge> edges;
    QStringList groups;       // node (host) names, index = GraphNode::group
    int rankNodeCount = 0;    // nodes before the communicators
    qint64 windowNs = 0;      // length of the window the graph covers

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

// Communication pattern of the P2P edges of a rank-level graph (GraphLevel::Ranks): which
// partners the ranks have, as rank offsets d = (receiver - sender) mod n. Offsets that at least
// half of the sending ranks use are "regular"; the kind follows from them:
// Ring: only ±1 (or +1 / -1 alone: a pipeline); Stencil2D: ±1 and ±k with 1 < k < n/2 (a k-wide
// grid, gridColumns = k); AllToAll: the median rank talks to at least half of the others;
// Irregular: anything else; None: no P2P messages.
struct CommPattern {
    enum class Kind : quint8 { None, Ring, Pipeline, Stencil2D, AllToAll, Irregular };
    Kind kind = Kind::None;
    QVector<int> offsets;      // regular offsets, as signed values in (-n/2, n/2]
    int gridColumns = 0;       // Stencil2D: k
    int medianPartners = 0;    // distinct receivers of the median sending rank
    int sendingRanks = 0;
};
CommPattern detectPattern(const CommGraph &rankGraph);

// Findings of a rank-level graph of one window, in the order they should be shown: the
// communication pattern, how much P2P traffic stays inside the compute nodes, the ranks that wait
// most (outliers against the mean, as wait states in Scalasca: late sender / wait at
// collective) and the ranks that make others wait (senders of the late-sender time, i.e. the
// likely cause), the link with the most waiting. kind: "pattern", "locality", "waiting",
// "cause", "link"; rank: the rank the finding is about (-1 if none); text: one line for the GUI.
struct GraphFinding {
    QString kind;
    // "structure", "waiting", "load", "messages" or "comparison" (cards are grouped by it).
    QString category;
    QString action;     // what to do about it (empty: nothing to do)
    // Links to highlight (sender rank, receiver rank), e.g. a wait chain.
    QVector<QPair<int, int>> links;
    QString text;       // one line: title, number and explanation together
    int rank = -1;      // the rank the finding is mainly about
    double value = 0;
    // For the GUI: short title, the key number, one sentence; severity 0 = information,
    // 1 = worth a look, 2 = problem. ranks / nodes: what to highlight; fromRank / toRank: a link.
    QString title;
    QString metric;
    QString detail;
    int severity = 0;
    QVector<int> ranks;
    QStringList nodes;
    int fromRank = -1;
    int toRank = -1;
};
QVector<GraphFinding> analyzeCommGraph(const CommGraph &rankGraph, int gridColumns = 0);

// Comparison of a window with a reference window (both rank-level graphs of the same ranks):
// change of the total waiting, the ranks and links whose waiting grew / shrank most.
QVector<GraphFinding> compareCommGraphs(const CommGraph &current, const CommGraph &reference);

// Rank -> compute node assignment that keeps more P2P volume inside the nodes: block tiles of
// the process grid (gridColumns > 0) and a greedy grouping by communication, nodes of the
// current size. Returns the share of the volume inside nodes now and with the best found
// assignment, and its description ("" if none better).
struct MappingSuggestion {
    double currentShare = 0;
    double bestShare = 0;
    QString how;
    QVector<int> nodeOfRank; // indexed by graph node (rank level), the suggested node index
};
MappingSuggestion suggestMapping(const CommGraph &rankGraph, int gridColumns = 0);

// Waiting per graph node (seconds): late sender of P2P edges at their receiver, waiting at
// collectives at the calling rank / host. Communicators get 0.
QVector<double> nodeWaiting(const CommGraph &graph);

// Positions in the unit square [-1, 1]² (y down), one per graph node.
// Circle: rank/host nodes on a circle in graph order (ranks grouped by node, as in the
// overview), communicators on a small inner circle (one at the centre).
// Grid: rank/host nodes row by row in a near-square grid, communicators in a row below it.
// gridColumns > 0 (Grid only): ranks at (rank % gridColumns, rank / gridColumns) by their rank
// number, e.g. the process grid of a detected 2D stencil; other nodes row by row after them.
// previous (data-driven layouts, optional): positions of the same nodes to start from, so that
// a similar graph keeps a similar picture.
QVector<QPointF> layoutGraph(const CommGraph &graph, GraphLayout layout, int gridColumns = 0,
                             const QVector<QPointF> *previous = nullptr);

QString layoutName(GraphLayout layout);

// Layout chosen from the data: every candidate (Stress, Grid of a detected stencil, Circle,
// SpectralCircle, Spectral) is computed on the backbone of the P2P edges (per node its
// kBackbone heaviest edges; collectives left out, so that an MPI_Allreduce does not pull
// everything together) and scored by neighbourhood preservation: the share of each node's
// graph neighbours that are also among its nearest nodes in the picture (1 = every neighbour
// next to it; nodes on top of each other count against it) and by distance faithfulness
// (1 - normalised stress against the hop distances). Candidates with less than 40 % of the
// best preservation drop out (a folded torus, a collapsed picture); of the rest the most
// faithful wins, a later one (order above) only when better by 0.02. score = faithfulness.
struct LayoutChoice {
    GraphLayout layout = GraphLayout::Circle;
    QVector<QPointF> positions;
    double score = 0;
    QVector<std::pair<GraphLayout, double>> scores; // all candidates
    quint64 signature = 0;                          // of the backbone (same structure, same value)
};
constexpr int kBackbone = 6;
LayoutChoice autoLayout(const CommGraph &graph, int gridColumns = 0, const QVector<QPointF> *previous = nullptr,
                        const std::atomic_bool *cancel = nullptr);
// Hash of the backbone's edges (structure only, not the weights).
quint64 backboneSignature(const CommGraph &graph);
// Neighbourhood preservation of positions for the graph's backbone (0 .. 1).
double neighbourhoodPreservation(const CommGraph &graph, const QVector<QPointF> &positions);

} // namespace trace
