import QtQuick
import QtQuick.Controls
import QtQuick.Layouts 6.3

// Sidebar panel of the trace window (Sidebar_Trace): display options of the Trace Timeline
// view, details of the last clicked bar, trace info, "Show window in 3D/2D/Matrix" and the
// memory budget setting. Shares its state with TraceTimeline.qml through the window's
// traceState object (Trace_Analysis.qml); uses no trace-only types.
Rectangle {
    id: trace_options
    width: 250
    implicitHeight: content.implicitHeight + 20
    height: implicitHeight
    color: "#383936"

    readonly property var bar: traceState ? traceState.lastBar : null

    function formatNumber(value, decimals) {
        return Number(value).toLocaleString(Qt.locale("en_US"), "f", decimals)
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
                text: qsTr("Trace:")
                topPadding: 8
                color: "#00FF00"
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#999999"
                text: !traceState || traceState.name === "" ? qsTr("Loading…")
                      : [qsTr("Name: %1").arg(traceState.name),
                         qsTr("Ranks: %1").arg(traceState.ranks),
                         qsTr("Duration: %1 s").arg(trace_options.formatNumber(traceState.durationSeconds, 3)),
                         qsTr("Unmatched messages: %1").arg(traceState.unmatched)].join("\n")
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

            Text {
                text: qsTr("Memory budget (MB, 0 = automatic):")
                topPadding: 8
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#00FF00"
            }
            TextField {
                id: budget_field
                Layout.fillWidth: true
                validator: IntValidator { bottom: 0; top: 1048576 }
                text: nodesList ? String(nodesList.traceMemoryBudgetMB()) : "0"
                onEditingFinished: {
                    var mb = parseInt(text)
                    if (isNaN(mb) || mb < 0)
                        mb = 0
                    nodesList.setTraceMemoryBudgetMB(mb)
                    text = String(nodesList.traceMemoryBudgetMB())
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: "#999999"
                font.pointSize: 10
                text: qsTr("Applies to the next trace load.")
            }
        }
    }
}
