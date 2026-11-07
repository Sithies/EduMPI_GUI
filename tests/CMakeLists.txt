find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(trace_tests
    tst_trace_build.cpp
)

target_link_libraries(trace_tests PRIVATE
    Qt6::Core
    Qt6::Test
    edumpi_trace
)

add_test(NAME trace_tests COMMAND trace_tests)
