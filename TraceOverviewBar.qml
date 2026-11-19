import QtQuick
import GUI_Cluster 1.0

// Bottom strip of the trace window while the "Trace Timeline" view is selected (instead of
// Bottom_Bar): overview of the whole run with the timeline's window. Uses trace-only types:
// loaded by Trace_Analysis.qml only when the window has a timeline (trace builds).
Rectangle {
    id: overview_bar
    color: "#4d4d4d"

    property var model: null
    property real viewStartNs: 0
    property real viewEndNs: 0
    // Window dragged / clicked in the overview (size kept, clamped to the trace).
    signal windowMoved(real startNs, real endNs)

    Text {
        id: caption
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 25
        leftPadding: 5
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: "#00FF00"
        text: qsTr("Whole run – click or drag to move the timeline window")
    }

    TraceOverviewItem {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: caption.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 5
        model: overview_bar.model
        viewStartNs: overview_bar.viewStartNs
        viewEndNs: overview_bar.viewEndNs
        onWindowMoved: (startNs, endNs) => overview_bar.windowMoved(startNs, endNs)
    }
}
