import QtQuick
import QtQuick.Controls
import QtQuick.Layouts 6.3
import QtQuick.Dialogs

// Sidebar panel: select an OTF2 trace (file or Score-P experiment folder) and open it
// in a separate Trace_Analysis window.
Rectangle {

    id: trace_load
    width: 250
    height: 200
    implicitHeight: 200
    color: "#383936"

    Rectangle {
        id: rectangle
        x: 5
        y: 5
        radius: 10
        width: parent.width - 10
        anchors.leftMargin:  5
        height: parent.height - 10
        border.width: 2
        border.color: "#00FF00"
        color: "transparent"

        ColumnLayout{
            width: rectangle.width
            anchors.fill: parent
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            anchors.margins: 5

            Text {
                id: headline
                topPadding: 6
                color: "#00FF00"
                text: qsTr("Load Trace")
            }

            Label {
                Layout.preferredWidth: parent.width
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Select an OTF2 trace (traces.otf2) or a Score-P experiment folder")
                color: "#999999"
            }

            GridLayout{
                columns: 2

                RadioButton{
                    id: fileButton
                    text: qsTr("OTF2 file")
                    checked: true
                    palette.buttonText: "#999999"
                    palette.base: "#333333"
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                RadioButton{
                    id: folderButton
                    text: qsTr("Folder")
                    checked: false
                    palette.buttonText: "#999999"
                    palette.base: "#333333"
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }

            GridLayout{
                implicitWidth: parent.width
                columns: 2
                Layout.alignment: Qt.AlignHCenter
                Button {
                    text: qsTr("Browse")
                    palette.button: "#666666"
                    palette.buttonText: "white"
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    onClicked: {
                        fileButton.checked ? fileDialog.open() : folderDialog.open()
                    }
                }
                TextField{
                    id: tracePathField
                    Layout.fillWidth: true
                    text: ""
                    placeholderText: qsTr("<trace path>")
                    background: Rectangle {
                        color: "#4d4d4d"
                    }
                }
            }

            FileDialog {
                id: fileDialog
                nameFilters: [qsTr("OTF2 trace (*.otf2)")]
                onAccepted: tracePathField.text = selectedFile
            }
            FolderDialog {
                id: folderDialog
                onAccepted: tracePathField.text = selectedFolder
            }

            Button {
                id: trace_load_button
                text: qsTr("Load")
                palette.button: "#666666"
                palette.buttonText: "white"
                Layout.alignment: Qt.AlignCenter
                Layout.topMargin: 5
                enabled: tracePathField.text.trim() !== ""

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }

                onClicked: trace_load.openTrace(tracePathField.text.trim())
            }
        }
    }

    function openTrace(path){
        var component = Qt.createComponent("/Trace_Analysis.qml");
        if(component.status === Component.Ready){
            var window = component.createObject(root, {"tracePath": path});
            window.x = root.x + (root.width - window.width) / 2;
            window.y = root.y + (root.height - window.height) / 2;
            window.show();
        } else if (component.status === Component.Error) {
            console.error("Failed to load component:", component.errorString());
        }
    }
}
