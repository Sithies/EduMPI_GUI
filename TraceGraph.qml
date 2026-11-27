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
    // Simple mode (default): level, zoom, legend and findings only; "More options" shows layout,
    // colour pickers, filters, reference and the collective stepping.
    property bool advanced: false

    // Opens finding i (card pinned, its ranks highlighted and zoomed to, its rank / link selected);
    // the same finding again closes it.
    function openFinding(i) {
        if (i < 0 || i >= graph.findingList.length)
            return
        if (findingsPanel.pinned === i) {
            findingsPanel.pinned = -1
            graph.highlightedFinding = -1
            graph.resetView()
            return
        }
        findingsPanel.pinned = i
        var f = graph.findingList[i]
        var c = Object.assign({}, findingsPanel.collapsed)
        c[f.category] = false
        findingsPanel.collapsed = c
        if (f.ranks.length > 0 || f.nodes.length > 0)
            graph.zoomToFinding(i)
        else
            graph.highlightedFinding = i
        var r = f.fromRank >= 0 ? f.fromRank : f.rank
        if (r >= 0 && traceState)
            traceState.selectedRank = r
        if (f.fromRank >= 0 && f.toRank >= 0 && traceState) {
            traceState.graphLinkFrom = f.fromRank
            traceState.graphLinkTo = f.toRank
        }
    }

    // Plain-language meaning of a finding kind (shown in the opened card).
    function explainKind(kind) {
        switch (kind) {
        case "pattern": return qsTr("Who talks to whom follows a regular scheme (e.g. every rank with its grid neighbours). Not a problem – it tells you how the program is organised.")
        case "hub": return qsTr("One rank exchanges messages with very many others (e.g. a master collecting results). It can become a bottleneck when the others have to wait for it.")
        case "locality": return qsTr("How much traffic stays inside a compute node (fast, shared memory) versus crossing the network between nodes (slower).")
        case "mapping": return qsTr("A different placement of the ranks on the compute nodes would keep more partners on the same node, so fewer messages cross the network.")
        case "waiting": return qsTr("Ranks sit idle inside MPI because the data they need is not there yet. That time is lost for computing.")
        case "outlier": return qsTr("A rank or node behaves clearly differently from the rest – often a slower node or a rank with more work.")
        case "imbalance": return qsTr("The work is not spread evenly: some ranks compute much longer, the others wait for them.")
        case "compute": return qsTr("How the time outside MPI (computing) is spread over the ranks; large differences mean uneven work.")
        case "cause": return qsTr("Late sender: the receiver is ready but the message is sent late, so the receiver waits. The sender is the cause – look at what it does before sending.")
        case "latereceiver": return qsTr("Late receiver: the sender has to wait until the receiver finally asks for the message (blocking send of a large message).")
        case "chain": return qsTr("Waiting passes on: A waits for B, B waits for C … The real cause is at the end of the chain.")
        case "link": return qsTr("The single connection between two ranks with the most waiting.")
        case "size": return qsTr("Many very small messages cost mostly latency; very large ones cost bandwidth. Bundling small messages often helps.")
        case "lateatcollective": return qsTr("In a collective (e.g. MPI_Allreduce) everybody waits for the last rank to arrive. The rank shown arrives late most often – it is the one to look at.")
        case "collectiveops": return qsTr("Which collective operations take the most time in this window.")
        case "critical": return qsTr("The chain of computing and hand-overs that decides how long the window takes. Speeding up anything off this path does not make the program faster.")
        }
        return qsTr("A change compared with the reference window you set.")
    }

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
                    visible: trace_graph_view.advanced
                    implicitHeight: trace_graph_view.embedded ? 18 : 22
                    implicitWidth: 170
                    font.pointSize: 9
                    // Tree and the spectral layouts only through Auto (rarely the best, hard to read).
                    readonly property var values: ["auto", "circle", "grid", "torus", "stress"]
                    model: [
                        qsTr("Layout: Auto") + (graph.layout === "auto" ? " (" + graph.effectiveLayout + ")" : ""),
                        qsTr("Layout: Circle"), qsTr("Layout: Grid"), qsTr("Layout: Torus"), qsTr("Layout: Stress")
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
                        + "Auto may also pick a tree (root at the top) or a spectral layout when they fit the data best.")
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
                        else if (graph.notableEdgeCount < graph.shownEdgeCount)
                            t += qsTr(" · heaviest %1 strong, the rest pale").arg(graph.notableEdgeCount)
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
                component ToolButtonText: Rectangle {
                    property alias label: tbText.text
                    property bool active: false
                    property string tip: ""
                    signal clicked()
                    width: tbText.implicitWidth + 12
                    height: trace_graph_view.embedded ? 18 : 22
                    radius: 3
                    color: active ? "#00a000" : (tbMouse.containsMouse ? "#666666" : "#5a5a5a")
                    Text { id: tbText; anchors.centerIn: parent; color: "white"; font.pointSize: 9 }
                    MouseArea { id: tbMouse; anchors.fill: parent; hoverEnabled: true; onClicked: parent.clicked() }
                    ToolTip.visible: tip !== "" && tbMouse.containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: tip
                }
                ToolButtonText {
                    objectName: "helpButton"
                    label: qsTr("? What am I looking at")
                    tip: qsTr("Explains the picture, the colours and the findings in plain words")
                    onClicked: helpPopup.open()
                }
                ToolButtonText {
                    objectName: "moreOptionsButton"
                    label: trace_graph_view.advanced ? qsTr("Fewer options ▴") : qsTr("More options ▾")
                    active: trace_graph_view.advanced
                    tip: qsTr("Layout, colour choices, filters, comparison with a reference window, stepping through collectives")
                    onClicked: trace_graph_view.advanced = !trace_graph_view.advanced
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
                    Text {
                        visible: !trace_graph_view.advanced
                        anchors.verticalCenter: parent.verticalCenter
                        text: graph.edgeLegend.title !== undefined ? graph.edgeLegend.title : ""
                        color: "#374151"
                        font.pointSize: 10
                    }
                    Picker {
                        visible: trace_graph_view.advanced
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
                        text: qsTr("· thicker = more %1 · dashed = collective%2")
                              .arg(graph.weight === "messages" ? qsTr("messages") : graph.weight === "wait" ? qsTr("waiting") : qsTr("bytes"))
                              .arg(graph.notableEdgeCount < graph.shownEdgeCount ? qsTr(" · pale grey = the lighter links") : "")
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
                    Text {
                        visible: !trace_graph_view.advanced
                        anchors.verticalCenter: parent.verticalCenter
                        text: graph.nodeLegend.title !== undefined ? graph.nodeLegend.title : ""
                        color: "#374151"
                        font.pointSize: 10
                    }
                    Picker {
                        visible: trace_graph_view.advanced
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
                    visible: trace_graph_view.advanced
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
                    visible: trace_graph_view.advanced
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
            maxEdges: trace_graph_view.traceState ? trace_graph_view.traceState.graphMaxEdges : 20000
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
                // In one sentence: how bad is it, and where to start (click opens that finding).
                Rectangle {
                    objectName: "summaryCard"
                    Layout.fillWidth: true
                    visible: graph.summary.text !== ""
                    Layout.preferredHeight: visible ? summaryText.implicitHeight + 14 : 0
                    radius: 6
                    readonly property int severity: graph.summary.severity !== undefined ? graph.summary.severity : 0
                    color: severity >= 2 ? "#fef2f2" : severity === 1 ? "#fffbeb" : "#f0fdf4"
                    border.color: severity >= 2 ? "#dc2626" : severity === 1 ? "#f59e0b" : "#16a34a"
                    border.width: summaryMouse.containsMouse ? 2 : 1
                    Text {
                        id: summaryText
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 8
                        wrapMode: Text.WordWrap
                        font.pointSize: 10
                        color: "#111827"
                        text: graph.summary.text + (graph.summary.finding >= 0 ? qsTr("  → show") : "")
                    }
                    MouseArea {
                        id: summaryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: graph.summary.finding >= 0
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: trace_graph_view.openFinding(graph.summary.finding)
                    }
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
                                                visible: card.expanded
                                                text: qsTr("What does this mean? ") + trace_graph_view.explainKind(card.f.kind)
                                                font.pointSize: 9
                                                font.italic: true
                                                color: "#1e3a8a"
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
                                            onClicked: trace_graph_view.openFinding(card.index)
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

    // "What am I looking at?": the view in plain words, for people new to MPI performance analysis.
    Popup {
        id: helpPopup
        objectName: "helpPopup"
        parent: trace_graph_view
        anchors.centerIn: parent
        width: Math.min(trace_graph_view.width - 40, 640)
        height: Math.min(trace_graph_view.height - 40, 560)
        modal: true
        padding: 14
        background: Rectangle { color: "#ffffff"; radius: 8; border.color: "#d1d5db" }
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: qsTr("What am I looking at?")
                    font.bold: true
                    font.pointSize: 13
                    color: "#111827"
                }
                Button { text: qsTr("Close"); onClicked: helpPopup.close() }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    textFormat: Text.StyledText
                    font.pointSize: 10
                    color: "#1f2937"
                    text: qsTr("<b>The picture</b><br>"
                        + "Every <b>circle</b> is one MPI process (rank) – or, when zoomed out on big runs, one compute node with all its ranks. "
                        + "Every <b>line</b> means: these two exchanged messages in the time window above the timeline. "
                        + "Thicker line = more data. Dashed lines go to a ◇, which stands for a collective operation (e.g. MPI_Allreduce) of a group of ranks.<br><br>"
                        + "<b>The colours</b><br>"
                        + "Circles: white = hardly waits, dark blue = waits a lot for others. "
                        + "Lines: green = nobody waits on this link, yellow = some waiting, red = a lot. "
                        + "With many links only the most important ones are drawn strong; the rest stay pale grey, so you still see the whole structure.<br><br>"
                        + "<b>How to read it</b><br>"
                        + "A healthy program looks calm: mostly white circles and green lines. "
                        + "Dark circles and red lines show where time is lost. "
                        + "If only a few ranks are dark, they wait for someone – the finding “late sender” or “wait chain” tells you for whom.<br><br>"
                        + "<b>Typical shapes</b><br>"
                        + "• Grid or torus: every rank talks to its neighbours (e.g. a simulation on a mesh).<br>"
                        + "• Ring: each rank passes data on to the next one.<br>"
                        + "• Star: one rank talks to all (master/worker) – can become a bottleneck.<br>"
                        + "• Clusters: groups that mostly talk among themselves.<br>"
                        + "The layout is chosen automatically so that partners sit next to each other.<br><br>"
                        + "<b>The findings (right)</b><br>"
                        + "Start with the sentence at the top: it says how much time is lost waiting and where to start. "
                        + "Red = problem, orange = worth a look, grey = information. Hover a card to light up its ranks, click it to zoom there and read what it means and what to do.<br><br>"
                        + "<b>Words</b><br>"
                        + "• <b>Late sender</b>: the receiver is ready, the message comes late – the receiver waits.<br>"
                        + "• <b>Late receiver</b>: the sender has to wait until the receiver asks for the message.<br>"
                        + "• <b>Collective</b>: an operation of many ranks together; all wait for the last one to arrive.<br>"
                        + "• <b>Load imbalance</b>: some ranks have more work than others, the rest wait.<br>"
                        + "• <b>Critical path</b>: the chain of work that decides the run time – only speeding up this chain helps.<br><br>"
                        + "<b>Using it</b><br>"
                        + "Mouse wheel / pinch to zoom, drag to move, Fit for everything. Click a circle to select the rank (also in the timeline), click a line for its details. "
                        + "Ranks / Nodes switches between single processes and compute nodes. “More options” has layouts, other colourings, filters, comparison of two windows and stepping through collective calls.")
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
