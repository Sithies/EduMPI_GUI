import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GUI_Cluster 1.0

// "Trace Graph" view of the trace window: communication graph (TraceGraphItem) of the current
// trace window, ranks as nodes, messages as edges, collectives as star edges to a communicator.
// Used twice: as its own view (Options_Views, traceMode) and below the timeline
// (TraceTimeline.qml, embedded = true). Window, selected rank and display options live in the
// window's traceState (Trace_Analysis.qml), shared with the timeline and Trace_Options.qml.
// Uses trace-only types: loaded only when the window's Cluster_Architecture has a timeline.
Rectangle {
    id: trace_graph_view
    color: "#ffffff"

    property var listNodes: null
    property QtObject traceState: null
    // Below the timeline: smaller toolbar, the timeline handles the overview bar.
    property bool embedded: false

    readonly property var timelineModel: listNodes ? listNodes.timeline : null

    function formatBytes(bytes) {
        var units = ["B", "KB", "MB", "GB", "TB"]
        var u = 0
        while (bytes >= 1024 && u < units.length - 1) {
            bytes /= 1024
            ++u
        }
        return bytes.toFixed(u === 0 ? 0 : 1) + " " + units[u]
    }

    // Own view: the overview bar moves the window (in the timeline view the timeline does).
    Connections {
        target: trace_graph_view.traceState
        enabled: !trace_graph_view.embedded
        function onViewRequested(startNs, endNs) {
            var len = trace_graph_view.timelineModel ? trace_graph_view.timelineModel.traceLengthNs : 0
            var span = endNs - startNs
            if (len > 0) {
                span = Math.min(Math.max(span, 10000), len)
                startNs = Math.min(Math.max(startNs, 0), len - span)
            }
            trace_graph_view.traceState.viewStartNs = startNs
            trace_graph_view.traceState.viewEndNs = startNs + span
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Toolbar: window, edge count, legend.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: trace_graph_view.embedded ? 24 : 34
            color: "#4d4d4d"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 5
                anchors.rightMargin: 5
                spacing: 12

                Text {
                    color: "#00FF00"
                    text: qsTr("Trace Graph")
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: "#cccccc"
                    text: {
                        if (graph.edgeCount === 0)
                            return ""
                        var t = qsTr("%1 edges").arg(graph.edgeCount)
                        if (graph.level === "auto" && graph.effectiveLevel === "nodes")
                            t += qsTr(" · compute nodes (zoom in for ranks)")
                        if (graph.communicatorsMerged)
                            t += qsTr(" · communicators merged")
                        if (graph.shownEdgeCount < graph.edgeCount)
                            t += qsTr(" (heaviest %1 shown)").arg(graph.shownEdgeCount)
                        if (graph.maxWaitSeconds > 0)
                            t += qsTr(" · max. waiting per rank %1 ms").arg((graph.maxWaitSeconds * 1000).toFixed(3))
                        return t
                    }
                }
                Text {
                    color: "#cccccc"
                    text: {
                        var s = trace_graph_view.traceState
                        if (s && s.viewEndNs > s.viewStartNs)
                            return qsTr("%1 – %2 s").arg((s.viewStartNs / 1e9).toFixed(6)).arg((s.viewEndNs / 1e9).toFixed(6))
                        return qsTr("whole run")
                    }
                }
            }
        }

        // Legend.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: legend.implicitHeight + 8
            color: "#f3f4f6"

            Flow {
                id: legend
                anchors.fill: parent
                anchors.margins: 4
                spacing: 14

                // Colours as in the 3D view (Cores3D.qml): P2P green, collectives black,
                // waiting per process white to blue. New: communicator node, host border.
                Repeater {
                    model: [
                        { label: qsTr("P2P message"), colour: "#00d500", dashed: false },
                        { label: qsTr("Collective → communicator"), colour: "#000000", dashed: true }
                    ]
                    delegate: Row {
                        required property var modelData
                        spacing: 4
                        Canvas {
                            width: 22
                            height: 12
                            anchors.verticalCenter: parent.verticalCenter
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                ctx.strokeStyle = modelData.colour
                                ctx.lineWidth = 3
                                if (modelData.dashed)
                                    ctx.setLineDash([2, 1.5])
                                ctx.beginPath()
                                ctx.moveTo(1, height / 2)
                                ctx.lineTo(width - 1, height / 2)
                                ctx.stroke()
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: "#374151"
                            font.pointSize: 11
                        }
                    }
                }
                Row {
                    spacing: 4
                    Canvas {
                        width: 14
                        height: 14
                        anchors.verticalCenter: parent.verticalCenter
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.reset()
                            ctx.fillStyle = "#d1d5db"
                            ctx.strokeStyle = "#1f2937"
                            ctx.lineWidth = 1
                            ctx.beginPath()
                            ctx.moveTo(7, 1)
                            ctx.lineTo(13, 7)
                            ctx.lineTo(7, 13)
                            ctx.lineTo(1, 7)
                            ctx.closePath()
                            ctx.fill()
                            ctx.stroke()
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Communicator")
                        color: "#374151"
                        font.pointSize: 11
                    }
                }
                Row {
                    spacing: 4
                    visible: graph.effectiveLevel === "ranks"
                    Rectangle {
                        width: 12
                        height: 12
                        radius: 6
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#ffffff"
                        border.color: "#f28e2b"
                        border.width: 2
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Border = compute node")
                        color: "#374151"
                        font.pointSize: 11
                    }
                }
                // Node fill: waiting time (late sender) of the rank, as the 3D view's scale.
                Row {
                    spacing: 4
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Waiting (late sender): 0")
                        color: "#374151"
                        font.pointSize: 11
                    }
                    Rectangle {
                        width: 70
                        height: 10
                        anchors.verticalCenter: parent.verticalCenter
                        border.color: "#9ca3af"
                        border.width: 1
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "#ffffff" }
                            GradientStop { position: 1.0; color: "#0000ff" }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("%1 ms").arg((graph.maxWaitSeconds * 1000).toFixed(3))
                        color: "#374151"
                        font.pointSize: 11
                    }
                }
                Text {
                    text: {
                        var w = graph.weight === "messages" ? qsTr("number of messages")
                              : graph.weight === "wait" ? qsTr("waiting time") : qsTr("bytes")
                        return qsTr("Edge width: %1").arg(w)
                    }
                    color: "#374151"
                    font.pointSize: 11
                }
            }
        }

        TraceGraphItem {
            id: graph
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: trace_graph_view.timelineModel
            viewStartNs: trace_graph_view.traceState ? trace_graph_view.traceState.viewStartNs : 0
            viewEndNs: trace_graph_view.traceState ? trace_graph_view.traceState.viewEndNs : 0
            selectedRank: trace_graph_view.traceState ? trace_graph_view.traceState.selectedRank : -1
            showP2P: trace_graph_view.traceState ? trace_graph_view.traceState.graphP2P : true
            showCollectives: trace_graph_view.traceState ? trace_graph_view.traceState.graphCollectives : true
            directed: trace_graph_view.traceState ? trace_graph_view.traceState.graphDirected : true
            weight: trace_graph_view.traceState ? trace_graph_view.traceState.graphWeight : "bytes"
            level: trace_graph_view.traceState ? trace_graph_view.traceState.graphLevel : "auto"
            communicators: trace_graph_view.traceState ? trace_graph_view.traceState.graphCommunicators : "auto"
            layout: trace_graph_view.traceState ? trace_graph_view.traceState.graphLayout : "circle"
            maxEdges: trace_graph_view.traceState ? trace_graph_view.traceState.graphMaxEdges : 500

            onSelectedRankChanged: {
                if (trace_graph_view.traceState && trace_graph_view.traceState.selectedRank !== selectedRank)
                    trace_graph_view.traceState.selectedRank = selectedRank
            }
            onEdgeClicked: (details) => {
                if (trace_graph_view.traceState)
                    trace_graph_view.traceState.lastEdge = details
            }

            // Click: node selects the rank (also in the timeline), edge shows its details.
            TapHandler {
                onTapped: (eventPoint, button) => graph.clickAt(eventPoint.position.x, eventPoint.position.y)
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: trace_graph_view.timelineModel === null || trace_graph_view.timelineModel.rankCount === 0
        text: qsTr("Loading trace…")
        color: "#374151"
    }
}
