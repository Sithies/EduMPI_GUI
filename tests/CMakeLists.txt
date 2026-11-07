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
