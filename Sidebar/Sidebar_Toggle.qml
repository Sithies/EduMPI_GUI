import QtQuick
import QtQuick.Controls

// Narrow strip at the left edge of a sidebar: click folds the sidebar in (only the strip stays,
// the view next to it gets the room) or out again.
Rectangle {
    id: toggle
    property bool collapsed: false
    signal toggled()

    width: 18
    color: toggleMouse.containsMouse ? "#4d4d4b" : "#2e2f2c"

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        text: toggle.collapsed ? "‹" : "›"
        color: "#00FF00"
        font.pixelSize: 18
        font.bold: true
    }
    MouseArea {
        id: toggleMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: toggle.toggled()
    }
    ToolTip.visible: toggleMouse.containsMouse
    ToolTip.delay: 500
    ToolTip.text: toggle.collapsed ? qsTr("Show the sidebar") : qsTr("Hide the sidebar")
}
