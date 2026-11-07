# OTF2 trace reading support (static library). Only added when EDUMPI_TRACE=ON.

set(OTF2_ROOT "$ENV{HOME}/.local/otf2" CACHE PATH
    "OTF2 install prefix containing bin/otf2-config (see scripts/build-otf2.sh)")

find_program(OTF2_CONFIG otf2-config
    PATHS "${OTF2_ROOT}/bin"
    NO_DEFAULT_PATH)
if(NOT OTF2_CONFIG)
    message(FATAL_ERROR
        "otf2-config not found in ${OTF2_ROOT}/bin. Run scripts/build-otf2.sh <prefix> "
        "and pass -DOTF2_ROOT=<prefix>, or configure with -DEDUMPI_TRACE=OFF.")
endif()

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
    trace_aggregator.h
    trace_aggregator.cpp
    otf2_trace_reader.h
    otf2_trace_reader.cpp
    ${PROJECT_SOURCE_DIR}/data_column.h
    trace_data_source.h
    trace_data_source.cpp
)

# PROJECT_SOURCE_DIR: shared app headers without heavy dependencies (data_column.h).
target_include_directories(edumpi_trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_compile_options(edumpi_trace PUBLIC ${OTF2_CFLAGS})
target_link_libraries(edumpi_trace PUBLIC Qt6::Core ${OTF2_LDFLAGS} ${OTF2_LIBS})
