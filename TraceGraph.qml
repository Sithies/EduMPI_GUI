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

    // Next / previous collective call: view around it, its last arriver selected; none left in
    // the loaded window: the window moves on by its length (click again once loaded).
    function stepCollective(direction) {
        if (!traceState)
            return
        var r = graph.stepCollective(direction)
        if (r.found) {
            collectiveLabel.text = r.label
            traceState.selectedRank = r.lastRank
            // Circles show the arrival at this call (white = first, red = last).
            if (traceState.graphNodeColor !== undefined)
                traceState.graphNodeColor = "arrival"
            else
                graph.nodeColor = "arrival"
            traceState.viewRequested(r.fromNs, r.toNs)
            return
        }
        var span = Math.max(traceState.viewEndNs - traceState.viewStartNs, 1000000)
        var start = direction > 0 ? traceState.viewEndNs : traceState.viewStartNs - span
        collectiveLabel.text = direction > 0 ? qsTr("no further collective here – moved on, click again")
                                             : qsTr("no earlier collective here – moved back, click again")
        traceState.viewRequested(Math.max(0, start), Math.max(0, start) + span)
    }

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
                // Layout: Auto (chosen from the data) or a fixed one, shared with Trace Options.
                ComboBox {
                    id: layoutBox
                    implicitHeight: trace_graph_view.embedded ? 18 : 22
                    implicitWidth: 170
                    font.pointSize: 9
                    readonly property var values: ["auto", "circle", "grid", "torus", "tree", "stress", "spectral", "spectral circle"]
                    model: [
                        qsTr("Layout: Auto") + (graph.layout === "auto" ? " (" + graph.effectiveLayout + ")" : ""),
                        qsTr("Layout: Circle"), qsTr("Layout: Grid"), qsTr("Layout: Torus"), qsTr("Layout: Tree"), qsTr("Layout: Stress"),
                        qsTr("Layout: Spectral"), qsTr("Layout: Spectral circle")
                    ]
                    currentIndex: Math.max(0, values.indexOf(graph.layout))
                    onActivated: (index) => {
                        if (trace_graph_view.traceState)
                            trace_graph_view.traceState.graphLayout = values[index]
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Auto computes several layouts from who talks to whom and keeps the one where communication partners end up next to each other:\n"
                        + "• Circle: ranks in order, grouped by compute node\n"
                        + "• Grid: the process grid of a detected 2D stencil\n"
                        + "• Stress: distances in the picture follow the hops between ranks (grids become grids, rings rings, clusters clusters)\n"
                        + "• Torus: a periodic grid (edges wrap around) cut open flat, found from the data; links across the cut are short dotted stubs at the border\n"
                        + "• Tree: root at the top, levels below – for tree-shaped communication (reductions, broadcast trees)\n"
                        + "• Spectral: from the eigenvectors of the communication graph\n"
                        + "• Spectral circle: a circle in which partners sit next to each other")
                }
                // Zoom: − / + / Fit (also mouse wheel or pinch; drag to move).
                Row {
                    spacing: 2
                    Repeater {
                        model: [
                            { label: "−", tip: qsTr("Zoom out (mouse wheel / pinch)"), act: "out" },
                            { label: "+", tip: qsTr("Zoom in (mouse wheel / pinch); drag to move"), act: "in" },
                            { label: qsTr("Fit"), tip: qsTr("Show the whole graph"), act: "fit" }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            width: Math.max(22, zoomText.implicitWidth + 10)
                            height: trace_graph_view.embedded ? 18 : 22
                            radius: 3
                            color: zoomMouse.containsMouse ? "#666666" : "#5a5a5a"
                            Text {
                                id: zoomText
                                anchors.centerIn: parent
                                text: modelData.label
                                color: "white"
                                font.pointSize: 10
                                font.bold: true
                            }
                            MouseArea {
                                id: zoomMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    if (modelData.act === "fit")
                                        graph.resetView()
                                    else
                                        graph.zoomAt(graph.width / 2, graph.height / 2, modelData.act === "in" ? 1.5 : 1 / 1.5)
                                }
                            }
                            ToolTip.visible: zoomMouse.containsMouse
                            ToolTip.delay: 500
                            ToolTip.text: modelData.tip
                        }
                    }
                    Text {
                        visible: graph.zoom > 1.01
                        anchors.verticalCenter: parent.verticalCenter
                        leftPadding: 4
                        text: graph.zoom.toFixed(1) + "×"
                        color: "#cccccc"
                        font.pointSize: 9
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
                            t += (graph.effectiveLevel === "nodes" ? qsTr(" · max. waiting per node %1 ms") : qsTr(" · max. waiting per rank %1 ms"))
                                 .arg((graph.maxWaitSeconds * 1000).toFixed(3))
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

        // Colours and filters: what the lines and circles mean, and where to switch it.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: legend.implicitHeight + 8
            color: "#f3f4f6"

            component LegendScale: Row {
                property var info: ({})
                spacing: 4
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: info.low !== undefined ? info.low : ""
                    color: "#374151"
                    font.pointSize: 10
                }
                Rectangle {
                    width: 70
                    height: 10
                    anchors.verticalCenter: parent.verticalCenter
                    border.color: "#9ca3af"
                    border.width: 1
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: info.colors && info.colors.length > 0 ? info.colors[0] : "white" }
                        GradientStop { position: 0.5; color: info.colors && info.colors.length > 2 ? info.colors[1] : (info.colors && info.colors.length > 1 ? Qt.tint(info.colors[0], Qt.rgba(0, 0, 0, 0)) : "white") }
                        GradientStop { position: 1.0; color: info.colors && info.colors.length > 0 ? info.colors[info.colors.length - 1] : "white" }
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: info.high !== undefined ? info.high : ""
                    color: "#374151"
                    font.pointSize: 10
                }
            }
            component Picker: ComboBox {
                property var values: []
                property string current: ""
                signal picked(string value)
                implicitHeight: 22
                font.pointSize: 10
                currentIndex: Math.max(0, values.indexOf(current))
                onActivated: (index) => picked(values[index])
            }

            Flow {
                id: legend
                anchors.fill: parent
                anchors.margins: 4
                spacing: 14

                // Lines: colour by …
                Row {
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Lines:")
                        font.bold: true
                        color: "#111827"
                        font.pointSize: 10
                    }
                    Picker {
                        implicitWidth: 170
                        values: graph.hasReference ? ["wait", "volume", "size", "locality", "change"] : ["wait", "volume", "size", "locality"]
                        model: graph.hasReference
                               ? [qsTr("colour: waiting"), qsTr("colour: volume"), qsTr("colour: message size"), qsTr("colour: inside / between nodes"), qsTr("colour: vs. reference")]
                               : [qsTr("colour: waiting"), qsTr("colour: volume"), qsTr("colour: message size"), qsTr("colour: inside / between nodes")]
                        current: graph.edgeColor
                        onPicked: (v) => { if (trace_graph_view.traceState) trace_graph_view.traceState.graphEdgeColor = v; else graph.edgeColor = v }
                    }
                    LegendScale {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: graph.edgeLegend.categories === undefined
                        info: graph.edgeLegend
                    }
                    Repeater {
                        model: graph.edgeLegend.categories !== undefined ? graph.edgeLegend.categories : []
                        delegate: Row {
                            required property var modelData
                            spacing: 3
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle { width: 18; height: 4; color: modelData.color; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: modelData.label; color: "#374151"; font.pointSize: 10 }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("· width: %1 · strong = between nodes, light = inside a node · dashed = collective")
                              .arg(graph.weight === "messages" ? qsTr("messages") : graph.weight === "wait" ? qsTr("waiting") : qsTr("bytes"))
                        color: "#6b7280"
                        font.pointSize: 10
                    }
                }
                // Circles: fill by …
                Row {
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Circles:")
                        font.bold: true
                        color: "#111827"
                        font.pointSize: 10
                    }
                    Picker {
                        implicitWidth: 150
                        values: ["wait", "compute", "ratio", "arrival"]
                        model: [qsTr("fill: waiting"), qsTr("fill: compute time"), qsTr("fill: sent / received"), qsTr("fill: arrival at collective")]
                        current: graph.nodeColor
                        onPicked: (v) => { if (trace_graph_view.traceState) trace_graph_view.traceState.graphNodeColor = v; else graph.nodeColor = v }
                    }
                    LegendScale {
                        anchors.verticalCenter: parent.verticalCenter
                        info: graph.nodeLegend
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: graph.effectiveLevel === "nodes" ? qsTr("· green ring = messages inside the node · ◇ = communicator")
                                                               : qsTr("· border colour = compute node · ◇ = communicator")
                        color: "#6b7280"
                        font.pointSize: 10
                    }
                }
                // Filter, rank search, reference.
                Row {
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Show:")
                        font.bold: true
                        color: "#111827"
                        font.pointSize: 10
                    }
                    Picker {
                        implicitWidth: 170
                        values: ["all", "waiting", "between", "inside"]
                        model: [qsTr("all lines"), qsTr("only lines with waiting"), qsTr("only between nodes"), qsTr("only inside nodes")]
                        current: graph.edgeFilter
                        onPicked: (v) => { if (trace_graph_view.traceState) trace_graph_view.traceState.graphEdgeFilter = v; else graph.edgeFilter = v }
                    }
                    TextField {
                        id: rankSearch
                        implicitWidth: 90
                        implicitHeight: 22
                        font.pointSize: 10
                        placeholderText: qsTr("Rank …")
                        validator: IntValidator { bottom: 0 }
                        onAccepted: {
                            var r = parseInt(text)
                            if (!isNaN(r) && graph.zoomToRank(r) && trace_graph_view.traceState)
                                trace_graph_view.traceState.selectedRank = r
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Rank number + Enter: zoom to it and select it")
                    }
                    Button {
                        implicitHeight: 22
                        font.pointSize: 10
                        text: graph.hasReference ? qsTr("Clear reference") : qsTr("Set as reference")
                        onClicked: graph.hasReference ? graph.clearReference() : graph.setReference()
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: graph.hasReference
                                      ? qsTr("Comparing with the window %1; findings under “Comparison”, lines can be coloured by the change").arg(graph.referenceLabel)
                                      : qsTr("Remember this window; then move to another window to compare (findings and line colour “vs. reference”)")
                    }
                    Text {
                        visible: graph.hasReference
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Reference: %1").arg(graph.referenceLabel)
                        color: "#1d4ed8"
                        font.pointSize: 10
                    }
                }
                // Step from collective call to collective call (window follows; the rank that
                // arrived last is selected). At the end of the loaded window the window moves on.
                Row {
                    spacing: 4
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Collectives:")
                        font.bold: true
                        color: "#111827"
                        font.pointSize: 10
                    }
                    Button {
                        implicitHeight: 22
                        implicitWidth: 30
                        text: "◀"
                        onClicked: trace_graph_view.stepCollective(-1)
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Previous collective call")
                    }
                    Button {
                        implicitHeight: 22
                        implicitWidth: 30
                        text: "▶"
                        onClicked: trace_graph_view.stepCollective(1)
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Next collective call")
                    }
                    Button {
                        implicitHeight: 22
                        checkable: true
                        checked: graph.showCriticalPath
                        text: qsTr("Critical path")
                        onToggled: graph.showCriticalPath = checked
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Draws the critical path of the window in gold: the chain of work and hand-overs that determines its run time (time split in the finding “Critical path”)")
                    }
                    Text {
                        id: collectiveLabel
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, 520)
                        elide: Text.ElideRight
                        text: graph.collectiveCount > 0 ? qsTr("%1 calls in this window").arg(graph.collectiveCount) : qsTr("none in this window")
                        color: "#374151"
                        font.pointSize: 10
                    }
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
                        + "• Layout Auto: chosen from the data so that partners sit next to each other (see the Layout box).\n"
                        + "• Zoom: mouse wheel or pinch, drag to move, Fit to see everything; rank numbers appear when zoomed in far enough.\n"
                        + "• Findings (right): communication pattern, locality, waiting, slow nodes, late senders. Hover one to highlight its ranks, click to zoom there and select the rank.\n"
                        + "Hover a circle or line for its numbers; double-click a node to open it into its ranks.")
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

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
            layout: trace_graph_view.traceState && trace_graph_view.traceState.graphLayout ? trace_graph_view.traceState.graphLayout : "auto"
            maxEdges: trace_graph_view.traceState ? trace_graph_view.traceState.graphMaxEdges : 500
            edgeColor: trace_graph_view.traceState && trace_graph_view.traceState.graphEdgeColor ? trace_graph_view.traceState.graphEdgeColor : "wait"
            nodeColor: trace_graph_view.traceState && trace_graph_view.traceState.graphNodeColor ? trace_graph_view.traceState.graphNodeColor : "wait"
            edgeFilter: trace_graph_view.traceState && trace_graph_view.traceState.graphEdgeFilter ? trace_graph_view.traceState.graphEdgeFilter : "all"

            onSelectedRankChanged: {
                if (trace_graph_view.traceState && trace_graph_view.traceState.selectedRank !== selectedRank)
                    trace_graph_view.traceState.selectedRank = selectedRank
            }
            // A clicked link: details in Trace Options, its messages highlighted in the timeline
            // (same window, orange arrows). Clicking a circle or empty space ends it.
            onEdgeClicked: (details) => {
                if (!trace_graph_view.traceState)
                    return
                trace_graph_view.traceState.lastEdge = details
                if (details.kind === "p2p" && details.fromRank >= 0 && details.toRank >= 0) {
                    trace_graph_view.traceState.graphLinkFrom = details.fromRank
                    trace_graph_view.traceState.graphLinkTo = details.toRank
                }
            }

            // Click: node selects the rank (also in the timeline), edge shows its details.
            // Double-click (nodes level): open a compute node into its ranks / merge it again.
            TapHandler {
                onTapped: (eventPoint, button) => {
                    if (trace_graph_view.traceState && trace_graph_view.traceState.graphLinkFrom !== undefined) {
                        trace_graph_view.traceState.graphLinkFrom = -1
                        trace_graph_view.traceState.graphLinkTo = -1
                    }
                    graph.clickAt(eventPoint.position.x, eventPoint.position.y)
                }
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

            // Zoom: wheel / trackpad scroll around the pointer, pinch; drag moves the view.
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    var d = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y * 2
                    graph.zoomAt(point.position.x, point.position.y, Math.pow(1.0015, d))
                }
            }
            PinchHandler {
                id: pinch
                target: null
                property real last: 1
                onActiveChanged: last = 1
                onActiveScaleChanged: {
                    graph.zoomAt(centroid.position.x, centroid.position.y, activeScale / last)
                    last = activeScale
                }
            }
            DragHandler {
                target: null
                property point last: Qt.point(0, 0)
                onActiveChanged: last = Qt.point(0, 0)
                onTranslationChanged: {
                    graph.panBy(translation.x - last.x, translation.y - last.y)
                    last = Qt.point(translation.x, translation.y)
                }
            }
        }

        // Findings of the window, grouped by category (collapsible). A card shows title and key
        // number; hover highlights its ranks / links in the graph, a click opens it (explanation,
        // what to do, waiting over time) and zooms there; click again closes it.
        Rectangle {
            id: findingsPanel
            objectName: "findingsPanel"
            Layout.fillHeight: true
            Layout.preferredWidth: visible ? (trace_graph_view.embedded ? 280 : 340) : 0
            visible: graph.findingList.length > 0 && trace_graph_view.width > 700
            color: "#f9fafb"
            border.color: "#e5e7eb"
            property int pinned: -1
            property var collapsed: ({})
            readonly property var categories: [
                { key: "critical", label: qsTr("Critical path") },
                { key: "waiting", label: qsTr("Waiting") },
                { key: "collectives", label: qsTr("Collectives") },
                { key: "load", label: qsTr("Load") },
                { key: "structure", label: qsTr("Structure") },
                { key: "messages", label: qsTr("Messages") },
                { key: "comparison", label: qsTr("Comparison") }
            ]
            function indicesOf(cat) {
                var out = []
                for (var i = 0; i < graph.findingList.length; ++i)
                    if (graph.findingList[i].category === cat)
                        out.push(i)
                return out
            }
            function worst(cat) {
                var w = 0
                var idx = indicesOf(cat)
                for (var i = 0; i < idx.length; ++i)
                    w = Math.max(w, graph.findingList[idx[i]].severity)
                return w
            }

            Connections {
                target: graph
                function onGraphChanged() {
                    if (findingsPanel.pinned >= graph.findingList.length)
                        findingsPanel.pinned = -1
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 4
                Text {
                    text: qsTr("Findings in this window")
                    font.bold: true
                    font.pointSize: 11
                    color: "#111827"
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Hover to highlight, click to open and zoom there.")
                    font.pointSize: 9
                    color: "#6b7280"
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    Column {
                        width: parent.width
                        spacing: 6
                        Repeater {
                            model: findingsPanel.categories
                            delegate: Column {
                                id: section
                                required property var modelData
                                readonly property var items: findingsPanel.indicesOf(modelData.key)
                                readonly property bool open: !findingsPanel.collapsed[modelData.key]
                                visible: items.length > 0
                                width: parent.width
                                spacing: 4
                                // Category header: name, count, worst severity; click folds it.
                                Rectangle {
                                    width: parent.width
                                    height: 22
                                    radius: 4
                                    color: headMouse.containsMouse ? "#e5e7eb" : "#eef0f3"
                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        x: 6
                                        spacing: 6
                                        Text { text: section.open ? "▾" : "▸"; color: "#374151"; font.pointSize: 10 }
                                        Text { text: section.modelData.label; font.bold: true; color: "#111827"; font.pointSize: 10 }
                                        Rectangle {
                                            width: countText.implicitWidth + 10
                                            height: 16
                                            radius: 8
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: findingsPanel.worst(section.modelData.key) >= 2 ? "#dc2626"
                                                 : findingsPanel.worst(section.modelData.key) === 1 ? "#f59e0b" : "#9ca3af"
                                            Text { id: countText; anchors.centerIn: parent; text: section.items.length; color: "white"; font.pointSize: 9; font.bold: true }
                                        }
                                    }
                                    MouseArea {
                                        id: headMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: {
                                            var c = Object.assign({}, findingsPanel.collapsed)
                                            c[section.modelData.key] = section.open
                                            findingsPanel.collapsed = c
                                        }
                                    }
                                }
                                Repeater {
                                    model: section.open ? section.items : []
                                    delegate: Rectangle {
                                        id: card
                                        required property int modelData
                                        readonly property int index: modelData
                                        readonly property var f: graph.findingList[modelData]
                                        readonly property bool expanded: findingsPanel.pinned === index
                                        readonly property bool active: expanded || cardMouse.containsMouse
                                        readonly property color severityColor: f.severity >= 2 ? "#dc2626" : f.severity === 1 ? "#f59e0b" : "#9ca3af"
                                        width: section.width
                                        height: cardColumn.implicitHeight + 12
                                        radius: 6
                                        color: active ? "#fff7ed" : "#ffffff"
                                        border.color: active ? "#f97316" : "#e5e7eb"
                                        border.width: active ? 2 : 1
                                        Rectangle {
                                            width: 4
                                            radius: 2
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            anchors.margins: 3
                                            color: card.severityColor
                                        }
                                        ColumnLayout {
                                            id: cardColumn
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.top: parent.top
                                            anchors.leftMargin: 14
                                            anchors.rightMargin: 8
                                            anchors.topMargin: 6
                                            spacing: 2
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6
                                                Text {
                                                    text: {
                                                        switch (card.f.kind) {
                                                        case "pattern": return "◇"
                                                        case "hub": return "✳"
                                                        case "locality": return "⌂"
                                                        case "mapping": return "⇆"
                                                        case "waiting": return "◷"
                                                        case "outlier": return "▲"
                                                        case "imbalance": return "⚖"
                                                        case "compute": return "⚙"
                                                        case "cause": return "⇢"
                                                        case "latereceiver": return "⇠"
                                                        case "chain": return "⛓"
                                                        case "link": return "⇄"
                                                        case "size": return "✉"
                                                        case "lateatcollective": return "⏱"
                                                        case "critical": return "★"
                                                        case "collectiveops": return "∑"
                                                        }
                                                        return "≠"
                                                    }
                                                    color: card.severityColor
                                                    font.pointSize: 12
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: card.f.title
                                                    font.bold: true
                                                    font.pointSize: 10
                                                    color: "#111827"
                                                    elide: Text.ElideRight
                                                }
                                                Text { text: card.expanded ? "▾" : "▸"; color: "#9ca3af"; font.pointSize: 9 }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: card.f.metric
                                                font.pointSize: 11
                                                font.bold: true
                                                color: card.f.severity >= 2 ? "#b91c1c" : "#1f2937"
                                                wrapMode: Text.WordWrap
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                visible: card.expanded
                                                text: card.f.detail
                                                font.pointSize: 9
                                                color: "#4b5563"
                                                wrapMode: Text.WordWrap
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                visible: card.expanded && card.f.action !== ""
                                                text: qsTr("What to do: ") + card.f.action
                                                font.pointSize: 9
                                                color: "#065f46"
                                                wrapMode: Text.WordWrap
                                            }
                                            // Waiting over time (window in 40 steps): these ranks vs. all.
                                            Canvas {
                                                id: spark
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 36
                                                visible: card.expanded && card.f.series.length > 1
                                                onVisibleChanged: requestPaint()
                                                onWidthChanged: requestPaint()
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    var s = card.f.series, b = card.f.baseline
                                                    if (!s || s.length < 2)
                                                        return
                                                    var max = 1e-9
                                                    for (var i = 0; i < s.length; ++i)
                                                        max = Math.max(max, s[i], b && b.length > i ? b[i] : 0)
                                                    function line(v, colour, w) {
                                                        ctx.strokeStyle = colour
                                                        ctx.lineWidth = w
                                                        ctx.beginPath()
                                                        for (var k = 0; k < v.length; ++k) {
                                                            var x = k / (v.length - 1) * (width - 2) + 1
                                                            var y = height - 12 - v[k] / max * (height - 16)
                                                            if (k === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                                                        }
                                                        ctx.stroke()
                                                    }
                                                    if (b && b.length === s.length)
                                                        line(b, "#9ca3af", 1)
                                                    line(s, "#f97316", 2)
                                                    ctx.fillStyle = "#6b7280"
                                                    ctx.font = "9px sans-serif"
                                                    ctx.fillText(qsTr("waiting over the window: these ranks (orange) vs. all (grey)"), 1, height - 1)
                                                }
                                            }
                                        }
                                        MouseArea {
                                            id: cardMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onContainsMouseChanged: {
                                                if (findingsPanel.pinned < 0)
                                                    graph.highlightedFinding = containsMouse ? card.index : -1
                                            }
                                            onClicked: {
                                                if (findingsPanel.pinned === card.index) {
                                                    findingsPanel.pinned = -1
                                                    graph.highlightedFinding = -1
                                                    graph.resetView()
                                                    return
                                                }
                                                findingsPanel.pinned = card.index
                                                var f = card.f
                                                if (f.ranks.length > 0 || f.nodes.length > 0)
                                                    graph.zoomToFinding(card.index)
                                                else
                                                    graph.highlightedFinding = card.index
                                                var r = f.fromRank >= 0 ? f.fromRank : f.rank
                                                if (r >= 0 && trace_graph_view.traceState)
                                                    trace_graph_view.traceState.selectedRank = r
                                                if (f.fromRank >= 0 && f.toRank >= 0 && trace_graph_view.traceState) {
                                                    trace_graph_view.traceState.graphLinkFrom = f.fromRank
                                                    trace_graph_view.traceState.graphLinkTo = f.toRank
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
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
