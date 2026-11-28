import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GUI_Cluster 1.0

// "Trace Timeline" view of the trace window: per-rank timeline (TraceTimelineItem) with
// toolbar, legend and hints. Uses trace-only types: loaded (Options_Views, traceMode) only when
// the window's Cluster_Architecture has a timeline, i.e. only in trace builds.
// View range, display options, the last clicked bar and the time cursor are kept in the
// window's traceState (Trace_Analysis.qml), shared with Trace_Options.qml, the overview bar and
// the other trace views.
Rectangle {
    id: trace_timeline_view
    color: "#ffffff"

    property var listNodes: null
    property QtObject traceState: null

    readonly property var timelineModel: listNodes ? listNodes.timeline : null
    // Rows below the 22 px time axis.
    readonly property real visibleRowsHeight: Math.max(0, timeline.height - 22) // kAxisHeight
    readonly property real maxContentY: Math.max(0, timeline.contentHeight - visibleRowsHeight)
    property bool restored: false
    // TraceTimelineItem::kLabelWidth / kAxisHeight.
    readonly property real labelWidth: 48

    function clampContentY(y) {
        return Math.min(Math.max(0, y), maxContentY)
    }
    // Alt + wheel (either direction): scroll the rows.
    function scrollRows(delta) {
        if (delta !== 0)
            timeline.contentY = clampContentY(timeline.contentY - delta / 2)
    }

    function syncState() {
        if (!traceState || !restored)
            return
        traceState.viewStartNs = timeline.viewStartNs
        traceState.viewEndNs = timeline.viewEndNs
    }

    onMaxContentYChanged: {
        if (timeline.contentY > maxContentY)
            timeline.contentY = maxContentY
    }

    Component.onCompleted: {
        if (traceState) {
            if (traceState.viewEndNs > traceState.viewStartNs)
                timeline.setView(traceState.viewStartNs, traceState.viewEndNs)
            timeline.selectedRank = traceState.selectedRank
        }
        restored = true
        syncState()
    }

    Connections {
        target: trace_timeline_view.traceState
        // Overview bar: window moved.
        function onViewRequested(startNs, endNs) {
            timeline.setView(startNs, endNs)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Toolbar: zoom buttons, hints, detail loading state.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#4d4d4d"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 5
                anchors.rightMargin: 5
                spacing: 6

                Button {
                    text: "−"
                    Layout.preferredWidth: 32
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Zoom out")
                    onClicked: timeline.zoomAt(trace_timeline_view.labelWidth + (timeline.width - trace_timeline_view.labelWidth) / 2, 2.0)
                }
                Button {
                    text: "+"
                    Layout.preferredWidth: 32
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Zoom in")
                    onClicked: timeline.zoomAt(trace_timeline_view.labelWidth + (timeline.width - trace_timeline_view.labelWidth) / 2, 0.5)
                }
                Button {
                    text: qsTr("Full run")
                    onClicked: timeline.showFullRun()
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: "#00FF00"
                    text: {
                        var hints = []
                        var m = trace_timeline_view.timelineModel
                        if (m && m.windowed && m.maxDetailWindowNs > 0
                                && (timeline.viewEndNs - timeline.viewStartNs) > m.maxDetailWindowNs)
                            hints.push(qsTr("Zoom in to see messages and details"))
                        else if (m && m.detailOverBudget)
                            hints.push(qsTr("Too dense for the memory budget — zoom in further"))
                        if (timeline.showArrows && timeline.selectedRank === -1 && !timeline.allArrows)
                            hints.push(qsTr("Click a rank to see its messages"))
                        if (timeline.showArrows && timeline.arrowsCapped)
                            hints.push(qsTr("Too many messages — zoom in"))
                        return hints.join("  ·  ")
                    }
                }
                Text {
                    visible: trace_timeline_view.timelineModel !== null && trace_timeline_view.timelineModel.loadingDetail
                    color: "#ffffff"
                    text: qsTr("Loading details… %1 %").arg(trace_timeline_view.timelineModel ? trace_timeline_view.timelineModel.detailProgress : 0)
                }
                Text {
                    color: "#cccccc"
                    text: qsTr("%1 – %2 s")
                            .arg((timeline.viewStartNs / 1e9).toFixed(6))
                            .arg((timeline.viewEndNs / 1e9).toFixed(6))
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

                Repeater {
                    model: [
                        { label: qsTr("Computation"), colour: "#c9ced3", hatched: false },
                        { label: qsTr("P2P send"), colour: "#4caf50", hatched: false },
                        { label: qsTr("P2P receive"), colour: "#d9534f", hatched: false },
                        { label: qsTr("Collective"), colour: "#3b82f6", hatched: false },
                        { label: qsTr("Other MPI"), colour: "#6b7280", hatched: false },
                        { label: qsTr("Waiting (late sender)"), colour: "#e0b84a", hatched: true }
                    ]
                    delegate: Row {
                        required property var modelData
                        spacing: 4
                        Canvas {
                            width: 18
                            height: 12
                            anchors.verticalCenter: parent.verticalCenter
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                ctx.fillStyle = modelData.colour
                                ctx.fillRect(0, 0, width, height)
                                if (modelData.hatched) {
                                    ctx.strokeStyle = "#a07a10"
                                    ctx.lineWidth = 1
                                    ctx.beginPath()
                                    for (var x = -height; x < width; x += 4) {
                                        ctx.moveTo(x, height)
                                        ctx.lineTo(x + height, 0)
                                    }
                                    ctx.stroke()
                                }
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
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TraceTimelineItem {
                id: timeline
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: vbar.left
                model: trace_timeline_view.timelineModel
                showArrows: trace_timeline_view.traceState ? trace_timeline_view.traceState.showArrows : true
                allArrows: trace_timeline_view.traceState ? trace_timeline_view.traceState.allArrows : false
                highlightWait: trace_timeline_view.traceState ? trace_timeline_view.traceState.highlightWait : true
                showPhases: trace_timeline_view.traceState ? trace_timeline_view.traceState.showPhases : true
                phaseLevel: trace_timeline_view.traceState ? trace_timeline_view.traceState.phaseLevel : "outer"
                cursorNs: trace_timeline_view.traceState ? trace_timeline_view.traceState.cursorNs : -1
                cursorWidthNs: trace_timeline_view.traceState ? trace_timeline_view.traceState.stepNs : 0

                onViewChanged: trace_timeline_view.syncState()
                // Rows stay within [0, max(0, contentHeight − (height − 22))].
                onContentYChanged: {
                    var y = trace_timeline_view.clampContentY(contentY)
                    if (y !== contentY)
                        contentY = y
                }
                onSelectedRankChanged: {
                    if (trace_timeline_view.traceState && trace_timeline_view.restored)
                        trace_timeline_view.traceState.selectedRank = selectedRank
                }
                // Click into the rows: the cursor goes to the time under the pointer; a click on a
                // call then moves it to the call's start and selects its message (if matched).
                onTimeClicked: (ns, rank) => {
                    if (trace_timeline_view.traceState)
                        trace_timeline_view.traceState.cursorNs = ns
                }
                onBarClicked: (details) => {
                    var st = trace_timeline_view.traceState
                    if (!st)
                        return
                    st.lastBar = details
                    if (details.function !== "Computation")
                        st.cursorNs = details.startNs
                    st.selectedMessage = details.messageSender !== undefined
                            ? {sender: details.messageSender, receiver: details.messageReceiver,
                               sendNs: details.messageSendNs}
                            : null
                }

                // Wheel / two-finger swipe up-down: zoom around the pointer; swipe sideways (or a
                // tilt wheel) and Shift + wheel: move in time; Alt + wheel: scroll the rows.
                // WheelHandler takes only mice and only one direction by default, so trackpads
                // (macOS) and both directions are set explicitly. macOS turns Shift + mouse wheel
                // into a horizontal wheel, Linux keeps it vertical: both move in time.
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    acceptedModifiers: Qt.NoModifier
                    orientation: Qt.Vertical
                    onWheel: (event) => {
                        if (Math.abs(event.angleDelta.y) >= Math.abs(event.angleDelta.x) && event.angleDelta.y !== 0)
                            timeline.zoomAt(event.x, Math.pow(0.85, event.angleDelta.y / 120))
                    }
                }
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    acceptedModifiers: Qt.NoModifier
                    orientation: Qt.Horizontal
                    onWheel: (event) => {
                        var dx = event.angleDelta.x
                        if (Math.abs(dx) > Math.abs(event.angleDelta.y))
                            timeline.panBy(-(event.pixelDelta.x !== 0 ? event.pixelDelta.x : dx / 3))
                    }
                }
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    acceptedModifiers: Qt.ShiftModifier
                    orientation: Qt.Vertical
                    onWheel: (event) => timeline.panBy(-event.angleDelta.y / 3)
                }
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    acceptedModifiers: Qt.ShiftModifier
                    orientation: Qt.Horizontal
                    onWheel: (event) => timeline.panBy(-event.angleDelta.x / 3)
                }
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    acceptedModifiers: Qt.AltModifier
                    orientation: Qt.Vertical
                    onWheel: (event) => trace_timeline_view.scrollRows(event.angleDelta.y)
                }
                WheelHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    acceptedModifiers: Qt.AltModifier
                    orientation: Qt.Horizontal
                    onWheel: (event) => trace_timeline_view.scrollRows(event.angleDelta.x)
                }
                // Drag: pan in time (horizontal) and scroll the rows (vertical).
                DragHandler {
                    id: drag
                    target: null
                    property real lastX: 0
                    property real lastY: 0
                    onActiveChanged: {
                        lastX = 0
                        lastY = 0
                    }
                    onActiveTranslationChanged: {
                        var dx = activeTranslation.x - lastX
                        var dy = activeTranslation.y - lastY
                        lastX = activeTranslation.x
                        lastY = activeTranslation.y
                        if (dx !== 0)
                            timeline.panBy(-dx)
                        if (dy !== 0)
                            timeline.contentY = trace_timeline_view.clampContentY(timeline.contentY - dy)
                    }
                }
                // Click: label selects the rank, bar / phase band shows details.
                TapHandler {
                    onTapped: (eventPoint, button) => timeline.clickAt(eventPoint.position.x, eventPoint.position.y)
                }
            }

            ScrollBar {
                id: vbar
                orientation: Qt.Vertical
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 22
                anchors.bottom: parent.bottom
                policy: ScrollBar.AlwaysOn
                size: timeline.contentHeight > trace_timeline_view.visibleRowsHeight
                      ? trace_timeline_view.visibleRowsHeight / timeline.contentHeight : 1.0
                position: timeline.contentHeight > 0 ? timeline.contentY / timeline.contentHeight : 0
                onPositionChanged: {
                    if (pressed)
                        timeline.contentY = trace_timeline_view.clampContentY(position * timeline.contentHeight)
                }
            }

            Text {
                anchors.centerIn: timeline
                visible: trace_timeline_view.timelineModel === null || trace_timeline_view.timelineModel.rankCount === 0
                text: qsTr("Loading trace timeline…")
                color: "#374151"
            }
        }
    }
}
