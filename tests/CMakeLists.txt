find_package(Qt6 REQUIRED COMPONENTS Test)

# One Qt Test executable per test class (each with its own QTEST_GUILESS_MAIN),
# registered with CTest as trace_<name>. The umbrella target `trace_tests` builds all.
add_custom_target(trace_tests)

function(edumpi_add_trace_test name)
    qt_add_executable(tst_trace_${name} tst_trace_${name}.cpp ${ARGN})
    target_link_libraries(tst_trace_${name} PRIVATE Qt6::Core Qt6::Test edumpi_trace)
    add_test(NAME trace_${name} COMMAND tst_trace_${name})
    add_dependencies(trace_tests tst_trace_${name})
endfunction()

edumpi_add_trace_test(build)
edumpi_add_trace_test(aggregator ring4_scenario.h ring4_scenario.cpp)
edumpi_add_trace_test(otf2_reader
    otf2_fixture.h otf2_fixture.cpp
    ring4_scenario.h ring4_scenario.cpp)
edumpi_add_trace_test(data_source otf2_fixture.h otf2_fixture.cpp)
edumpi_add_trace_test(timeline_builder otf2_fixture.h otf2_fixture.cpp)
edumpi_add_trace_test(timeline_view_math otf2_fixture.h otf2_fixture.cpp)
edumpi_add_trace_test(graph_model otf2_fixture.h otf2_fixture.cpp)

# Cluster_Architecture fed by Trace_Data_Source: compiles the app classes it needs
# (Cluster_Architecture pulls in Database_Thread for the DB path) into the test.
edumpi_add_trace_test(cluster_architecture
    otf2_fixture.h otf2_fixture.cpp
    ${PROJECT_SOURCE_DIR}/cluster_architecture.h ${PROJECT_SOURCE_DIR}/cluster_architecture.cpp
    ${PROJECT_SOURCE_DIR}/cluster_node.h ${PROJECT_SOURCE_DIR}/cluster_node.cpp
    ${PROJECT_SOURCE_DIR}/cluster_rank.h ${PROJECT_SOURCE_DIR}/cluster_rank.cpp
    ${PROJECT_SOURCE_DIR}/detailed_p2p_data.h ${PROJECT_SOURCE_DIR}/detailed_p2p_data.cpp
    ${PROJECT_SOURCE_DIR}/detailed_coll_data.h ${PROJECT_SOURCE_DIR}/detailed_coll_data.cpp
    ${PROJECT_SOURCE_DIR}/database_thread.h ${PROJECT_SOURCE_DIR}/database_thread.cpp
    ${PROJECT_SOURCE_DIR}/database_connection.h ${PROJECT_SOURCE_DIR}/database_connection.cpp)
find_package(Qt6 REQUIRED COMPONENTS Gui Sql Qml Concurrent)
target_link_libraries(tst_trace_cluster_architecture PRIVATE Qt6::Gui Qt6::Sql Qt6::Qml Qt6::Concurrent)
target_compile_definitions(tst_trace_cluster_architecture PRIVATE EDUMPI_TRACE=1)

# Timeline / overview QQuickPaintedItems rendered into a QImage (no QML engine needed).
edumpi_add_trace_test(timeline_item
    otf2_fixture.h otf2_fixture.cpp
    ${PROJECT_SOURCE_DIR}/trace_timeline_item.h ${PROJECT_SOURCE_DIR}/trace_timeline_item.cpp
    ${PROJECT_SOURCE_DIR}/trace_overview_item.h ${PROJECT_SOURCE_DIR}/trace_overview_item.cpp
    ${PROJECT_SOURCE_DIR}/trace_graph_item.h ${PROJECT_SOURCE_DIR}/trace_graph_item.cpp)
find_package(Qt6 REQUIRED COMPONENTS Quick)
target_link_libraries(tst_trace_timeline_item PRIVATE Qt6::Gui Qt6::Quick)
set_tests_properties(trace_timeline_item PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Trace Graph item rendered into a QImage, and TraceGraph.qml / the graph below the timeline.
edumpi_add_trace_test(graph_item
    otf2_fixture.h otf2_fixture.cpp
    ${PROJECT_SOURCE_DIR}/trace_timeline_item.h ${PROJECT_SOURCE_DIR}/trace_timeline_item.cpp
    ${PROJECT_SOURCE_DIR}/trace_graph_item.h ${PROJECT_SOURCE_DIR}/trace_graph_item.cpp)
target_link_libraries(tst_trace_graph_item PRIVATE Qt6::Gui Qt6::Quick)
set_tests_properties(trace_graph_item PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Not a test: renders TraceGraph.qml for windows of a real trace into PNGs (tools/graph-snapshot).
qt_add_executable(trace_graph_snapshot
    ${PROJECT_SOURCE_DIR}/tools/graph-snapshot/graph_snapshot.cpp
    otf2_fixture.h otf2_fixture.cpp
    ${PROJECT_SOURCE_DIR}/trace_timeline_item.h ${PROJECT_SOURCE_DIR}/trace_timeline_item.cpp
    ${PROJECT_SOURCE_DIR}/trace_graph_item.h ${PROJECT_SOURCE_DIR}/trace_graph_item.cpp)
target_link_libraries(trace_graph_snapshot PRIVATE Qt6::Gui Qt6::Quick Qt6::Qml edumpi_trace)
target_compile_definitions(trace_graph_snapshot PRIVATE EDUMPI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(trace_graph_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
