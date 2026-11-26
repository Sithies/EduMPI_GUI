# OTF2 trace reading support (static library). Only added when the top-level CMakeLists found
# OTF2 (EDUMPI_TRACE_ENABLED); OTF2_CONFIG is the otf2-config it found.

function(_otf2_config out)
    execute_process(COMMAND "${OTF2_CONFIG}" ${ARGN}
        OUTPUT_VARIABLE _res
        RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "otf2-config ${ARGN} failed (${_rc})")
    endif()
    separate_arguments(_res UNIX_COMMAND "${_res}")
    set(${out} "${_res}" PARENT_SCOPE)
endfunction()

_otf2_config(OTF2_CFLAGS --cflags)
_otf2_config(OTF2_LDFLAGS --ldflags)
_otf2_config(OTF2_LIBS --libs)

add_library(edumpi_trace STATIC
    trace_version.h
    trace_version.cpp
    trace_records.h
    trace_sink.h
    trace_aggregator.h
    trace_aggregator.cpp
    timeline_store.h
    timeline_store.cpp
    timeline_builder.h
    timeline_builder.cpp
    otf2_trace_reader.h
    otf2_trace_reader.cpp
    evt_prefetch.h
    evt_prefetch.cpp
    ${PROJECT_SOURCE_DIR}/data_column.h
    trace_data_source.h
    trace_data_source.cpp
    memory_budget.h
    memory_budget.cpp
    timeline_snapshot.h
    trace_timeline_model.h
    trace_timeline_model.cpp
    timeline_view_math.h
    timeline_view_math.cpp
)

# PROJECT_SOURCE_DIR: shared app headers without heavy dependencies (data_column.h).
target_include_directories(edumpi_trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_compile_options(edumpi_trace PUBLIC ${OTF2_CFLAGS})
target_link_libraries(edumpi_trace PUBLIC Qt6::Core ${OTF2_LDFLAGS} ${OTF2_LIBS})
