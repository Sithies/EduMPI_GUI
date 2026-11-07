import QtQuick
import QtQuick.Controls
import QtQuick.Window
import GUI_Cluster 1.0
import Sidebar 1.0
import "."

// Trace window: same layout and window properties as MPI_Run_Analysis.qml (Bottom_Bar,
// Sidebar_Run_Analysis and the views read them by name), fed from an OTF2 trace instead
// of the database.
Window {
    id: run_analysis_win
    width: 1440
    height: 810
    visible: true
    title: qsTr("EduMPI – Trace")

    onClosing: {
        ascreen.clear();
        if(nodesList){
            bottom_main.close = true
            nodesList.destroy();
        }
    }

    Component.onCompleted: {
        nodesList.setOption(0)
        if(!nodesList.startTraceSource(tracePath)){
            showError(qsTr("Trace support is not available in this build."))
            // The opener calls show() right after createObject(); close afterwards.
            Qt.callLater(run_analysis_win.close)
        }
    }

    property string tracePath: ""
    property real traceDuration: 0
    property string traceHeader: qsTr("Trace: loading %1").arg(tracePath)

    property bool p2p: true
    property bool collective: true
    property string selected_screen: "Cores3D"
    property string option: "send/recv ratio (per proc)"
    property color gradient1: "green"
    property color gradient2: "red"
    property string gradient_text1: "Send"
    property string gradient_text2: "Recv"

    //Properties for timeline
    property bool enable_timeline
    property int startTime: 0
    property int endTime: 0
    property string loaderText: ""
    property bool visualization: true

    property bool enable_start: true
    property bool build_numbers: false

    property bool p2p_send_lines: false
    property bool p2p_recv_lines: false
    property bool coll_lines: false

    function showError(message){
        var component = Qt.createComponent("/Error.qml");
        if(component.status === Component.Ready){
            var window = component.createObject(root, {"message": message});
            window.x = run_analysis_win.x + (run_analysis_win.width - window.width) / 2;
            window.y = run_analysis_win.y + (run_analysis_win.height - window.height) / 2;
            window.show();
        } else if (component.status === Component.Error) {
            console.error("Failed to load component:", component.errorString());
        }
    }

    Item {
        id: run_analysis_win_Item
        anchors.fill: parent
        property int endTime: run_analysis_win.endTime
        property string option: run_analysis_win.option
        property string selected_screen: run_analysis_win.selected_screen

        property string functions: ""

        function flash() {
            opacityAnimation.start();
        }

        // Transparenz-Animation
        NumberAnimation {
            id: opacityAnimation
            target: run_analysis_win_Item
            property: "opacity"
            from: 1.0
            to: 0.1
            duration: 400 // Dauer des Aufblitzens (ms)
            onStopped: run_analysis_win_Item.opacity = 1.0 // Zurück zur Original-Transparenz
        }


        Cluster_Architecture{
            id: nodesList

            onComponentsBuilt: {
                busyindicator.running = false
                busyindicator.visible = false
                ascreen.push("Cores3D.qml", {listNodes: nodesList})
            }
            onTraceInfoReady: (name, ranks, durationSeconds, unmatched)=>{
                          run_analysis_win.traceDuration = durationSeconds
                          var header = qsTr("Trace: %1 · %2 ranks · %3 s").arg(name).arg(ranks).arg(durationSeconds.toFixed(1))
                          if(unmatched > 0){
                              header += qsTr(" · %1 unmatched messages").arg(unmatched)
                          }
                          run_analysis_win.traceHeader = header
                          run_analysis_win.title = qsTr("EduMPI – Trace %1").arg(name)
                      }
            onDataIn: (timestamp, qTimestamp, start)=>{
                          run_analysis_win.startTime = timestamp;
                          run_analysis_win.endTime = timestamp + Math.ceil(run_analysis_win.traceDuration);
                          run_analysis_win.enable_timeline = true;
                          nodesList.showConditionAt(0,0)
                      }
            onTraceLoadFailed: (message)=>{
                          run_analysis_win.showError(message)
                          run_analysis_win.close()
                      }
        }

        Rectangle{
            id: slurmnotifier
            width: parent.width
            height: 30
            anchors {
                top: parent.top
            }
            color: "#4d4d4d"
            Text {
                text: run_analysis_win.traceHeader
                verticalAlignment: Text.AlignVCenter
                leftPadding: 5
                width: parent.width
                height: parent.height
                elide: Text.ElideRight
                color: "#00FF00"
            }
        }

        StackView{
            id: ascreen
            property var map: []
            property int twoD_columns: 10
            property int matrix_zoom: 100
            property int threeD_depth: 2
            property bool reload: true

            anchors {
                left: parent.left
                right: options_analysis.left
                top: slurmnotifier.bottom
                bottom: bottom_main.top
            }
            width: run_analysis_win.width - options_analysis.width
        }

        Custom_BusyIndicator{
            id: busyindicator
            anchors.centerIn: ascreen
            prop_height: 40
            prop_width: 40
            prop_color: "#00FF00"
            running: true
        }

        Sidebar_Run_Analysis{
            id: options_analysis
            anchors.right: parent.right
            height: parent.height - bottom_main.height
            anchors.top: parent.top
        }
        Bottom_Bar{
            id: bottom_main
            live: false
            anchors.bottom: parent.bottom
        }
    }

}
