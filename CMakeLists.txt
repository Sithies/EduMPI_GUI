cmake_minimum_required(VERSION 3.19)

project(GUI_Cluster VERSION 1.0.0 LANGUAGES CXX)

# Release unless chosen from outside (-DCMAKE_BUILD_TYPE=..., multi-config generators):
# a forced Debug build made loading large traces about 7x slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(QT_QML_GENERATE_QMLLS_INI ON)

if(APPLE)
    set(PostgreSQL_ROOT "/opt/homebrew/opt/libpq" CACHE PATH "")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
#set(CMAKE_AUTOUIC ON)
#set(QT_QMLTYPE_REGISTRATION ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # AGL existiert auf modernen macOS SDKs nicht mehr
    set(WrapOpenGL_AGL "WrapOpenGL_AGL-NOTFOUND" CACHE FILEPATH "" FORCE)
endif()


# OTF2 trace loading support is optional. AUTO (default): built when OTF2 is found under
# OTF2_ROOT, otherwise the GUI builds without it. ON: OTF2 is required (CI, releases).
# OFF: never. Windows: always OFF (OTF2 does not support it officially).
set(EDUMPI_TRACE AUTO CACHE STRING "OTF2 trace loading support: AUTO, ON or OFF")
set_property(CACHE EDUMPI_TRACE PROPERTY STRINGS AUTO ON OFF)
if(WIN32)
    set(EDUMPI_TRACE OFF CACHE STRING "OTF2 trace loading support: AUTO, ON or OFF" FORCE)
endif()
set(OTF2_ROOT "$ENV{HOME}/.local/otf2" CACHE PATH
    "OTF2 install prefix containing bin/otf2-config (see scripts/build-otf2.sh)")

set(EDUMPI_TRACE_ENABLED OFF)
string(TOUPPER "${EDUMPI_TRACE}" _edumpi_trace_mode)
if(_edumpi_trace_mode STREQUAL "AUTO" OR EDUMPI_TRACE)
    find_program(OTF2_CONFIG otf2-config
        PATHS "${OTF2_ROOT}/bin"
        NO_DEFAULT_PATH)
    if(OTF2_CONFIG)
        set(EDUMPI_TRACE_ENABLED ON)
        message(STATUS "Trace support: on (OTF2 from ${OTF2_ROOT})")
    elseif(_edumpi_trace_mode STREQUAL "AUTO")
        message(STATUS "Trace support: off (otf2-config not found in ${OTF2_ROOT}/bin; "
                       "see README, \"Building with trace support\")")
    else()
        message(FATAL_ERROR
            "otf2-config not found in ${OTF2_ROOT}/bin. Run scripts/build-otf2.sh <prefix> "
            "and pass -DOTF2_ROOT=<prefix>, or configure with -DEDUMPI_TRACE=AUTO or OFF.")
    endif()
else()
    message(STATUS "Trace support: off (EDUMPI_TRACE=${EDUMPI_TRACE})")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Quick3D Sql Concurrent OpenGL)
find_package(PostgreSQL REQUIRED)

qt_standard_project_setup()

qt_policy(SET QTP0001 NEW)  # Automatisches Generieren von qmldir und qmltypes
qt_policy(SET QTP0002 NEW)  # Neues Verhalten für Build-/Installationsverzeichnis
qt_policy(SET QTP0003 NEW)  # Für Plugins, wenn benötigt
qt_policy(SET QTP0004 NEW)  # Module mit mehreren QML-Dateien benötigen ein qmldir

if (LINUX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
endif ()

#INCLUDE_DIRECTORIES("/usr/include/postgresql")
#LINK_DIRECTORIES("/usr/lib/postgresql/12/lib")
include_directories(${PostgreSQL_INCLUDE_DIRS})
link_directories(${PostgreSQL_LIBRARY_DIRS})

qt6_add_resources(QML_RESOURCES resources.qrc)

qt_add_executable(appGUI_Cluster
    main.cpp
    cluster_node.h
    cluster_node.cpp
    cluster_rank.h
    cluster_rank.cpp
    #controller.h
    #controller.cpp
    nodes_instances.h nodes_instances.cpp
    #ranks_instances.h ranks_instances.cpp
    data_column.h
    database_thread.h database_thread.cpp
    bash_process_manager.h bash_process_manager.cpp
    table_userid.h table_userid.cpp
    #cluster_architecture.h cluster_architecture.cpp
    database_connection.h database_connection.cpp
    ${QML_RESOURCES}
)

qt_add_qml_module(appGUI_Cluster
    URI GUI_Cluster
    VERSION 1.0
    RESOURCE_PREFIX "/"

    QML_FILES
        main.qml
        Cores2D.qml
        Cores3D.qml
        CommunicatorState.qml
        CoresCommunicationMatrix.qml
        Menu_Bar.qml
        Bottom_Bar.qml
        Cluster_Connection_Formular.qml
        DB_Connection_Formular.qml
        Key_Gen_Guide.qml
        Bash_Skript_Manual.qml
        Error.qml
        Custom_BusyIndicator.qml
        Loading.qml
        Output_File.qml
        Application_Start_without.qml
        Application_Start_Scorep.qml
        Table_Userid_Selection.qml
        MPI_Run_Analysis.qml
        Trace_Analysis.qml
        Application_Start_OpenMP.qml
        Detailed_Information_Bar.qml
    SOURCES
        controller.cpp
        controller.h
        cluster_architecture.cpp
        cluster_architecture.h
        detailed_p2p_data.h
        detailed_p2p_data.cpp
        ranks_instances.h
        ranks_instances.cpp
        customlinegeometry.h
        customlinegeometry.cpp
        detailed_coll_data.h
        detailed_coll_data.cpp
        heatmap_matrix.h
        heatmap_matrix.cpp
        heatmapmaterial.h
        heatmapmaterial.cpp

)

add_subdirectory(Sidebar)

if(EDUMPI_TRACE_ENABLED)
    add_subdirectory(trace)
    enable_testing()
    add_subdirectory(tests)
endif()


set_target_properties(appGUI_Cluster PROPERTIES
    #MACOSX_BUNDLE_GUI_IDENTIFIER my.example.com
    #MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    #MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    VS_DEBUGGER_ENVIRONMENT "QT_WAYLAND_DISABLE_TEXT_INPUT=1;QT_LOGGING_RULES=qt.qpa.wayland.textinput=false"
    QT_WAYLAND_DISABLE_TEXT_INPUT "1"

)

target_link_libraries(appGUI_Cluster PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Quick3D
    Qt6::Sql
    Qt6::Concurrent
    Qt6::OpenGL
)

# Dev-Modus für den lokalen Dev-Cluster (EduMPI/dev): Verbindungswerte aus EDUMPI_DEV_ENV,
# Auto-Connect. Nie in Release-Builds; aktiv erst, wenn EDUMPI_DEV_ENV gesetzt ist.
option(EDUMPI_DEV_MODE "Dev-Modus: Auto-Connect gegen den lokalen Dev-Cluster" OFF)
if(EDUMPI_DEV_MODE)
    target_compile_definitions(appGUI_Cluster PRIVATE EDUMPI_DEV_MODE)
endif()

if(EDUMPI_TRACE_ENABLED)
    target_link_libraries(appGUI_Cluster PRIVATE edumpi_trace)
    target_compile_definitions(appGUI_Cluster PRIVATE EDUMPI_TRACE=1)
    # Trace timeline QML types (QML_ELEMENT, module GUI_Cluster); absent without trace support.
    target_sources(appGUI_Cluster PRIVATE
        trace_timeline_item.h trace_timeline_item.cpp
        trace_overview_item.h trace_overview_item.cpp
        trace_graph_item.h trace_graph_item.cpp)
    # Views using those types; resources.qrc serves them as /TraceTimeline.qml,
    # /TraceGraph.qml and /TraceOverviewBar.qml, loaded only when a timeline exists.
    qt_target_qml_sources(appGUI_Cluster QML_FILES
        TraceTimeline.qml
        TraceGraph.qml
        TraceOverviewBar.qml)
endif()

install(TARGETS appGUI_Cluster
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

target_link_libraries(appGUI_Cluster PRIVATE
    sidebarplugin
)

if(APPLE)
    add_custom_command(TARGET appGUI_Cluster POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            /opt/homebrew/opt/libpq/lib/libpq.5.dylib
            $<TARGET_BUNDLE_DIR:appGUI_Cluster>/Contents/Frameworks/libpq.5.dylib
    )
endif()


