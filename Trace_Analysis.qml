import QtQuick
import QtQuick.Controls
import QtQuick.Window
import GUI_Cluster 1.0
import Sidebar 1.0
import "."

// Trace window: same layout and window properties as MPI_Run_Analysis.qml (Bottom_Bar,
// the sidebar and the views read them by name), fed from an OTF2 trace instead of the
// database. Sidebar_Trace adds the Trace Timeline and Trace Graph views and the Trace Options.
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
    // "Window: … s" after "Show window in 3D/2D/Matrix"; cleared when the bottom bar moves.
    property string traceWindowNote: ""

    // State of the Trace Timeline and Trace Graph views shared by TraceTimeline.qml,
    // TraceGraph.qml, TraceOverviewBar.qml and Sidebar/Trace_Options.qml (plain QML, no
    // trace-only types).
    property QtObject traceState: QtObject {
        property real viewStartNs: 0
        property real viewEndNs: 0
        property int selectedRank: -1
        property bool showArrows: true
        property bool allArrows: false
        property bool highlightWait: true
        property bool showPhases: true
        property string phaseLevel: "outer"
        property var lastBar: null
        // Trace Graph: shown below the timeline, display options, last clicked edge.
        property bool graphBelowTimeline: false
        property bool graphP2P: true
        property bool graphCollectives: true
        property bool graphDirected: true
        property string graphWeight: "bytes"     // "bytes", "messages" or "wait"
        property string graphLevel: "auto"       // "auto", "ranks" or "nodes"
        property string graphCommunicators: "auto" // "auto", "separate" or "merged"
        property string graphLayout: "circle"    // "circle" or "grid"
        property int graphMaxEdges: 500          // heaviest edges drawn, 0 = all
        property var lastEdge: null
        property string name: ""
        // Loaded file or folder as a local path (the file dialog yields file:// URLs).
        readonly property string path: decodeURIComponent(run_analysis_win.tracePath.replace(/^file:\/\//, ""))
        property int ranks: 0
        property real durationSeconds: 0
        property int unmatched: 0
        // Overview bar moved the window: the timeline sets both ends at once.
        signal viewRequested(real startNs, real endNs)
    }

    property bool p2p: true
    property bool collective: true
    property string selected_screen: "Cores3D"
    // Trace Timeline / Trace Graph: the overview bar replaces the bottom bar.
    readonly property bool traceView: selected_screen === "TraceTimeline" || selected_screen === "TraceGraph"
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

    // communicators of the selected second (filled by Cores3D) and the one selected in the
    // sidebar, as in main.qml / MPI_Run_Analysis.qml. The trace rows carry no communicator
    // name yet, so the list stays empty here.
    property var communicatorList: []
    property string selected_comm: ""

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
                // A view the user picked while loading is already on the stack: replace it
                // instead of pushing a second one (a hidden page would keep its connections).
                var pages = {TraceTimeline: "/TraceTimeline.qml", TraceGraph: "/TraceGraph.qml",
                             Cores2D: "/Cores2D.qml", CoresMatrix: "/CoresCommunicationMatrix.qml"}
                var page = pages[run_analysis_win.selected_screen] || "/Cores3D.qml"
                var props = run_analysis_win.traceView
                        ? {listNodes: nodesList, traceState: run_analysis_win.traceState}
                        : {listNodes: nodesList}
                if(ascreen.depth > 0){
                    ascreen.replace(page, props)
                } else {
                    ascreen.push(page, props)
                }
            }
            onTraceInfoReady: (name, ranks, durationSeconds, unmatched)=>{
                          run_analysis_win.traceDuration = durationSeconds
                          run_analysis_win.traceState.name = name
                          run_analysis_win.traceState.ranks = ranks
                          run_analysis_win.traceState.durationSeconds = durationSeconds
                          run_analysis_win.traceState.unmatched = unmatched
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
            onTraceWindowShown: (fromSeconds, toSeconds, exact)=>{
                          var note = qsTr("Window: %1 – %2 s").arg(fromSeconds.toFixed(3)).arg(toSeconds.toFixed(3))
                          if(!exact){
                              note += qsTr(" (whole seconds)")
                          }
                          run_analysis_win.traceWindowNote = note
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
                text: run_analysis_win.traceWindowNote === "" ? run_analysis_win.traceHeader
                      : run_analysis_win.traceHeader + " · " + run_analysis_win.traceWindowNote
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

        Sidebar_Trace{
            id: options_analysis
            anchors.right: parent.right
            height: parent.height - bottom_main.height
            anchors.top: parent.top
        }
        Bottom_Bar{
            id: bottom_main
            live: false
            anchors.bottom: parent.bottom
            // Replaced by the overview while the Trace Timeline or Trace Graph view is shown (kept
            // alive, so switching back restores it unchanged).
            visible: !run_analysis_win.traceView
        }
        // The bar (and its playback) is hidden in the Trace Timeline / Trace Graph view: pause
        // there, so it does not keep replacing the 3D/2D/Matrix data in the background.
        Connections {
            target: run_analysis_win
            function onSelected_screenChanged(){
                if(run_analysis_win.traceView){
                    bottom_main.pausePlayback()
                }
            }
        }
        Connections {
            target: nodesList
            // Data of another period is requested (Bottom_Bar: playback tick, timeline click,
            // step buttons, option change): the header's "Window: …" no longer applies.
            // showTraceWindow (the only source of the note) emits neither signal.
            function onSignalToUpdateData(){ run_analysis_win.traceWindowNote = "" }
            function onSignalToShowTimestampData(){ run_analysis_win.traceWindowNote = "" }
        }
        // TraceOverviewItem exists only in trace builds: loaded from a separate file and only
        // when the window has a timeline.
        Loader {
            id: trace_overview_loader
            x: bottom_main.x
            y: bottom_main.y
            width: bottom_main.width
            height: bottom_main.height
            active: run_analysis_win.traceView
                    && nodesList !== null && nodesList.timeline !== null
            visible: active
            source: "/TraceOverviewBar.qml"
            onLoaded: {
                item.model = Qt.binding(function(){ return nodesList ? nodesList.timeline : null })
                item.viewStartNs = Qt.binding(function(){ return run_analysis_win.traceState.viewStartNs })
                item.viewEndNs = Qt.binding(function(){ return run_analysis_win.traceState.viewEndNs })
                item.windowMoved.connect(function(startNs, endNs){
                    run_analysis_win.traceState.viewRequested(startNs, endNs)
                })
            }
        }
    }

}
