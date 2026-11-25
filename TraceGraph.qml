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
                // Level switch: Auto (by zoom) | Ranks | Nodes, shared with Trace Options.
                Row {
                    spacing: 2
                    Repeater {
                        model: [
                            { value: "auto", label: qsTr("Auto"), tip: qsTr("Ranks when zoomed in (window up to 50 ms) or for up to 64 ranks, compute nodes otherwise") },
                            { value: "ranks", label: qsTr("Ranks"), tip: qsTr("One circle per MPI rank") },
                            { value: "nodes", label: qsTr("Nodes"), tip: qsTr("One circle per compute node, its ranks merged") }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool active: graph.level === modelData.value
                            width: levelText.implicitWidth + 12
                            height: trace_graph_view.embedded ? 18 : 22
                            radius: 3
                            color: active ? "#00a000" : (levelMouse.containsMouse ? "#666666" : "#5a5a5a")
                            Text {
                                id: levelText
                                anchors.centerIn: parent
                                text: modelData.label + (modelData.value === "auto" && parent.active
                                      ? " (" + (graph.effectiveLevel === "nodes" ? qsTr("nodes") : qsTr("ranks")) + ")" : "")
                                color: "white"
                                font.pointSize: 9
                            }
                            MouseArea {
                                id: levelMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    if (trace_graph_view.traceState)
                                        trace_graph_view.traceState.graphLevel = modelData.value
                                }
                            }
                            ToolTip.visible: levelMouse.containsMouse
                            ToolTip.delay: 500
                            ToolTip.text: modelData.tip
                        }
                    }
                }
                Rectangle {
                    visible: graph.expandedCount > 0
                    width: collapseText.implicitWidth + 12
                    height: trace_graph_view.embedded ? 18 : 22
                    radius: 3
                    color: collapseMouse.containsMouse ? "#666666" : "#5a5a5a"
                    Text {
                        id: collapseText
                        anchors.centerIn: parent
                        text: qsTr("Merge %1 opened node(s)").arg(graph.expandedCount)
                        color: "white"
                        font.pointSize: 9
                    }
                    MouseArea {
                        id: collapseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: graph.collapseAll()
                    }
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: "#cccccc"
                    text: {
                        if (graph.edgeCount === 0)
                            return ""
                        var t = qsTr("%1 edges").arg(graph.edgeCount)
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
                Row {
                    spacing: 4
                    visible: graph.effectiveLevel === "nodes"
                    Rectangle {
                        width: 12
                        height: 12
                        radius: 6
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#ffffff"
                        border.color: "#00d500"
                        border.width: 3
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Green ring = messages inside the node")
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

        // What the picture shows (changes with the level) + ⓘ with the full explanation.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: explainRow.implicitHeight + 6
            color: "#ffffff"
            RowLayout {
                id: explainRow
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#374151"
                    font.pointSize: 10
                    text: graph.effectiveLevel === "nodes"
                          ? qsTr("Each circle is one compute node (all its ranks together). Lines: messages between ranks of different nodes. Green ring: messages inside the node. Double-click a node to show its ranks.")
                          : qsTr("Each circle is one MPI rank, border colour = its compute node. Lines: messages, arrow from sender to receiver. Click a rank to select it, hover for details.")
                }
                Rectangle {
                    width: 16
                    height: 16
                    radius: 8
                    color: infoMouse.containsMouse ? "#4a90e2" : "#9ca3af"
                    Text {
                        anchors.centerIn: parent
                        text: "i"
                        font.bold: true
                        font.pixelSize: 10
                        color: "white"
                    }
                    MouseArea {
                        id: infoMouse
                        anchors.fill: parent
                        hoverEnabled: true
                    }
                    ToolTip.visible: infoMouse.containsMouse
                    ToolTip.delay: 300
                    ToolTip.text: qsTr("Trace Graph: who talks to whom in the current time window.\n"
                        + "• Ranks: one circle per MPI process, grouped by compute node (border colour).\n"
                        + "• Nodes: the ranks of each compute node merged into one circle; only messages between nodes are lines, messages inside a node are the green ring (wider = more).\n"
                        + "• Green lines: point-to-point messages (width = bytes, messages or waiting time).\n"
                        + "• Black dashed lines: collectives (MPI_Allreduce, …) to a diamond per communicator.\n"
                        + "• Fill white → blue: waiting time (late sender), as in the 3D view.\n"
                        + "• Auto: ranks when zoomed in to single steps (≤ 50 ms) or for ≤ 64 ranks, nodes otherwise.\n"
                        + "Hover a circle or line for its numbers; double-click a node to open it into its ranks.")
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
            communicators: trace_graph_view.traceState && trace_graph_view.traceState.graphCommunicators ? trace_graph_view.traceState.graphCommunicators : "auto"
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
            // Double-click (nodes level): open a compute node into its ranks / merge it again.
            TapHandler {
                onTapped: (eventPoint, button) => graph.clickAt(eventPoint.position.x, eventPoint.position.y)
                onDoubleTapped: (eventPoint, button) => graph.toggleExpandAt(eventPoint.position.x, eventPoint.position.y)
            }

            // Hover: numbers of the node or edge under the mouse.
            HoverHandler {
                id: graphHover
                onPointChanged: {
                    var t = graph.toolTipAt(point.position.x, point.position.y)
                    if (t !== hoverTip.text)
                        hoverTip.text = t
                    hoverTip.x = Math.min(point.position.x + 14, Math.max(0, graph.width - hoverTip.width))
                    hoverTip.y = Math.min(point.position.y + 18, Math.max(0, graph.height - hoverTip.height))
                }
            }
            ToolTip {
                id: hoverTip
                visible: graphHover.hovered && text !== ""
                delay: 250
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
