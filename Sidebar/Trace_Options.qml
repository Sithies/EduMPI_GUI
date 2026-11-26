import QtQuick
import QtQuick.Controls
import QtQuick.Layouts 6.3

// Sidebar panel of the trace window (Sidebar_Trace): display options of the Trace Timeline
// and Trace Graph views, details of the last clicked bar / edge, trace info and "Show window
// in 3D/2D/Matrix" (the
// memory budget is set in Trace_Load.qml). Shares its state with TraceTimeline.qml through the window's
// traceState object (Trace_Analysis.qml); uses no trace-only types.
Rectangle {
    id: trace_options
    width: 250
    implicitHeight: content.implicitHeight + 20
    height: implicitHeight
    color: "#383936"

    readonly property var bar: traceState ? traceState.lastBar : null
    readonly property var edge: traceState ? traceState.lastEdge : null

    function formatBytes(bytes) {
        var units = ["B", "KB", "MB", "GB", "TB"]
        var u = 0
        while (bytes >= 1024 && u < units.length - 1) {
            bytes /= 1024
            ++u
        }
        return formatNumber(bytes, u === 0 ? 0 : 1) + " " + units[u]
    }

    function edgeLines() {
        if (!edge)
            return []
        var lines = []
        if (edge.kind === "collective")
            lines.push(qsTr("Collectives: %1 → %2").arg(edge.from).arg(edge.to))
        else
            lines.push(qsTr("Messages: %1 %2 %3").arg(edge.from).arg(edge.directed ? "→" : "↔").arg(edge.to))
        lines.push(qsTr("Count: %1").arg(edge.messages))
        lines.push(qsTr("Bytes: %1 (%2)").arg(formatNumber(edge.bytes, 0)).arg(formatBytes(edge.bytes)))
        lines.push(qsTr("Waiting (late sender): %1 ms").arg(formatNumber(edge.waitSeconds * 1000, 3)))
        lines.push(qsTr("Window: %1 – %2 s").arg(formatNumber(edge.fromNs / 1e9, 6)).arg(formatNumber(edge.toNs / 1e9, 6)))
        return lines
    }

    function formatNumber(value, decimals) {
        return Number(value).toLocaleString(Qt.locale("en_US"), "f", decimals)
    }

    // MB like the Memory budget field (8000 entered shows as 8,000 MB, not 7.8 GB).
    function formatMB(mb) {
        return qsTr("%1 MB").arg(formatNumber(mb, 0))
    }

    // Budget actually used by the load and the resulting mode (traceState, set from
    // Cluster_Architecture::traceMemoryInfo).
    function memoryLines() {
        if (!traceState || traceState.budgetMB <= 0)
            return [qsTr("Loading…")]
        var budget = formatMB(traceState.budgetMB)
        if (traceState.budgetSource === "automatic")
            budget = qsTr("%1 (automatic: half of %2 free)").arg(budget).arg(formatMB(traceState.availableMB))
        else if (traceState.budgetSource === "environment")
            budget = qsTr("%1 (from EDUMPI_TRACE_BUDGET_MB)").arg(budget)
        return [qsTr("Budget: %1").arg(budget),
                traceState.completeInMemory ? qsTr("Mode: complete in memory")
                                            : qsTr("Mode: overview, details loaded when zooming in")]
    }

    function barLines() {
        if (!bar)
            return []
        var lines = []
        lines.push(qsTr("Function: %1").arg(bar.function))
        // Why a MPI_Wait* waits: the nonblocking calls it completes.
        var completes = bar.completes ? Array.prototype.slice.call(bar.completes) : []
        if (completes.length > 0)
            lines.push(qsTr("Completes: %1").arg(completes.join(", ")))
        lines.push(qsTr("Rank: %1").arg(bar.rank))
        lines.push(qsTr("Start: %1 s").arg(formatNumber(bar.startUs / 1e6, 6)))
        lines.push(qsTr("Duration: %1 µs").arg(formatNumber(bar.durationUs, 1)))
        if (bar.waitUs > 0)
            lines.push(qsTr("Wait: %1 µs").arg(formatNumber(bar.waitUs, 1)))
        if (bar.partner >= 0)
            lines.push(qsTr("Partner: rank %1").arg(bar.partner))
        if (bar.partner >= 0 || bar.bytes > 0) {
            lines.push(qsTr("Bytes: %1").arg(formatNumber(bar.bytes, 0)))
            if (bar.partner >= 0)
                lines.push(qsTr("Tag: %1").arg(bar.tag))
            lines.push(qsTr("Communicator: %1").arg(bar.comm))
        }
        if (bar.messageCount > 1)
            lines.push(qsTr("Messages: %1").arg(bar.messageCount))
        var path = bar.callPath ? Array.prototype.slice.call(bar.callPath) : []
        if (path.length > 0)
            lines.push(qsTr("Call path: %1").arg(path.join(" ▸ ")))
        return lines
    }

    Rectangle {
        id: frame
        x: 5
        y: 5
        radius: 10
        width: parent.width - 10
        height: parent.height - 10
        border.width: 2
        border.color: "#00FF00"
        color: "transparent"

        ColumnLayout {
            id: content
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            spacing: 2

            Text {
                text: qsTr("Trace Timeline display:")
                topPadding: 4
                color: "#00FF00"
            }

            CheckBox {
                id: arrows_check
                text: qsTr("Arrows")
                checked: traceState ? traceState.showArrows : true
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.showArrows = checked
            }
            CheckBox {
                text: qsTr("All arrows")
                enabled: arrows_check.checked
                checked: traceState ? traceState.allArrows : false
                palette.base: "#333333"
                palette.windowText: "#999999"
                Layout.leftMargin: 20
                onToggled: traceState.allArrows = checked
            }
            CheckBox {
                text: qsTr("Highlight waiting")
                checked: traceState ? traceState.highlightWait : true
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.highlightWait = checked
            }
            CheckBox {
                id: phases_check
                text: qsTr("Phases")
                checked: traceState ? traceState.showPhases : true
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.showPhases = checked
            }
            RowLayout {
                Layout.leftMargin: 20
                enabled: phases_check.checked
                Text {
                    text: qsTr("Phase level:")
                    color: "#999999"
                }
                ComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Outermost"), qsTr("Innermost")]
                    currentIndex: traceState && traceState.phaseLevel === "inner" ? 1 : 0
                    onActivated: (index) => traceState.phaseLevel = index === 1 ? "inner" : "outer"
                }
            }

            Text {
                text: qsTr("Selected bar:")
                topPadding: 8
                color: "#00FF00"
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#999999"
                text: trace_options.bar ? trace_options.barLines().join("\n")
                                        : qsTr("Click a bar in the timeline to see its details.")
            }

            Text {
                text: qsTr("Trace Graph display:")
                topPadding: 8
                color: "#00FF00"
            }
            CheckBox {
                text: qsTr("Below the timeline")
                checked: traceState ? traceState.graphBelowTimeline : false
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.graphBelowTimeline = checked
            }
            CheckBox {
                text: qsTr("P2P messages")
                checked: traceState ? traceState.graphP2P : true
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.graphP2P = checked
            }
            CheckBox {
                text: qsTr("Collectives (star)")
                checked: traceState ? traceState.graphCollectives : true
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.graphCollectives = checked
            }
            CheckBox {
                text: qsTr("Direction (arrows)")
                checked: traceState ? traceState.graphDirected : true
                palette.base: "#333333"
                palette.windowText: "#999999"
                onToggled: traceState.graphDirected = checked
            }
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                columnSpacing: 6
                Text {
                    text: qsTr("Edge width:")
                    color: "#999999"
                }
                ComboBox {
                    Layout.fillWidth: true
                    readonly property var values: ["bytes", "messages", "wait"]
                    model: [qsTr("Bytes"), qsTr("Messages"), qsTr("Waiting time")]
                    currentIndex: traceState ? Math.max(0, values.indexOf(traceState.graphWeight)) : 0
                    onActivated: (index) => traceState.graphWeight = values[index]
                }
                Text {
                    text: qsTr("Nodes:")
                    color: "#999999"
                }
                ComboBox {
                    Layout.fillWidth: true
                    readonly property var values: ["auto", "ranks", "nodes"]
                    model: [qsTr("Auto (by zoom)"), qsTr("Ranks"), qsTr("Compute nodes")]
                    currentIndex: traceState ? Math.max(0, values.indexOf(traceState.graphLevel)) : 0
                    onActivated: (index) => traceState.graphLevel = values[index]
                }
                Text {
                    text: qsTr("Communicators:")
                    color: "#999999"
                }
                ComboBox {
                    Layout.fillWidth: true
                    readonly property var values: ["auto", "separate", "merged"]
                    model: [qsTr("Auto"), qsTr("One node each"), qsTr("One node for all")]
                    currentIndex: traceState ? Math.max(0, values.indexOf(traceState.graphCommunicators)) : 0
                    onActivated: (index) => traceState.graphCommunicators = values[index]
                }
                Text {
                    text: qsTr("Layout:")
                    color: "#999999"
                }
                ComboBox {
                    Layout.fillWidth: true
                    readonly property var values: ["auto", "circle", "grid", "torus", "tree", "stress", "spectral", "spectral circle"]
                    model: [qsTr("Auto (from the data)"), qsTr("Circle"), qsTr("Grid"), qsTr("Torus"), qsTr("Tree"), qsTr("Stress"), qsTr("Spectral"), qsTr("Spectral circle")]
                    currentIndex: traceState ? Math.max(0, values.indexOf(traceState.graphLayout)) : 0
                    onActivated: (index) => traceState.graphLayout = values[index]
                }
                Text {
                    text: qsTr("Edges:")
                    color: "#999999"
                }
                ComboBox {
                    Layout.fillWidth: true
                    readonly property var values: [100, 500, 2000, 0]
                    model: [qsTr("Heaviest 100"), qsTr("Heaviest 500"), qsTr("Heaviest 2000"), qsTr("All")]
                    currentIndex: traceState ? Math.max(0, values.indexOf(traceState.graphMaxEdges)) : 1
                    onActivated: (index) => traceState.graphMaxEdges = values[index]
                }
            }

            Text {
                text: qsTr("Selected edge:")
                topPadding: 8
                color: "#00FF00"
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#999999"
                text: trace_options.edge ? trace_options.edgeLines().join("\n")
                                         : qsTr("Click an edge in the Trace Graph to see its details.")
            }
            // Zooms the timeline / graph to the message of this link with the longest waiting
            // (a little before its send to a little after its receive).
            Button {
                visible: trace_options.edge !== null && trace_options.edge.longestSendNs !== undefined
                         && trace_options.edge.longestSendNs >= 0
                text: qsTr("Jump to the longest wait (%1 ms)")
                      .arg(trace_options.edge ? formatNumber(trace_options.edge.longestWaitSeconds * 1000, 3) : "")
                onClicked: {
                    var e = trace_options.edge
                    var span = Math.max(e.longestRecvNs - e.longestSendNs, 1000)
                    var pad = Math.max(span * 0.5, 20000)
                    traceState.graphLinkFrom = e.fromRank
                    traceState.graphLinkTo = e.toRank
                    traceState.viewRequested(Math.max(0, e.longestSendNs - pad), e.longestRecvNs + pad)
                }
            }

            Text {
                text: qsTr("Trace:")
                topPadding: 8
                color: "#00FF00"
            }
            Text {
                Layout.fillWidth: true
                // Paths have no spaces to break at.
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                color: "#999999"
                text: !traceState || traceState.name === "" ? qsTr("Loading…")
                      : [qsTr("Name: %1").arg(traceState.name),
                         qsTr("Path: %1").arg(traceState.path),
                         qsTr("Ranks: %1").arg(traceState.ranks),
                         qsTr("Duration: %1 s").arg(trace_options.formatNumber(traceState.durationSeconds, 3)),
                         qsTr("Unmatched messages: %1").arg(traceState.unmatched)].join("\n")
            }

            Text {
                text: qsTr("Memory:")
                topPadding: 8
                color: "#00FF00"
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#999999"
                text: trace_options.memoryLines().join("\n")
            }

            Button {
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: qsTr("Show window in 3D/2D/Matrix")
                enabled: traceState !== null && traceState.viewEndNs > traceState.viewStartNs
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
                onClicked: {
                    // A running playback would replace the window with its next second.
                    bottom_main.pausePlayback()
                    nodesList.showTraceWindow(traceState.viewStartNs / 1e9, traceState.viewEndNs / 1e9)
                    selected_screen = "Cores3D"
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#999999"
                font.pointSize: 10
                text: traceState && traceState.viewEndNs > traceState.viewStartNs
                      ? qsTr("Timeline window: %1 – %2 s")
                            .arg(trace_options.formatNumber(traceState.viewStartNs / 1e9, 6))
                            .arg(trace_options.formatNumber(traceState.viewEndNs / 1e9, 6))
                      : qsTr("Open the Trace Timeline view to choose a window.")
            }

        }
    }
}
