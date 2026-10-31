add_executable(hocket_stress_tests
    StressClientTests.cpp)

target_link_libraries(hocket_stress_tests PRIVATE hocket_stress_client hocket_api hocket_platform doctest::doctest_with_main)
target_include_directories(hocket_stress_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects from the source tree, as checked out.
target_compile_definitions(hocket_stress_tests PRIVATE HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

hocket_enable_warnings(hocket_stress_tests)

add_test(NAME hocket_stress_tests COMMAND hocket_stress_tests)

# hocket-stress itself, with a token file in a folder whose name is outside
# every Western ANSI code page, which doesn't exist: it must say so.
add_test(NAME hocket_stress_not_running
    COMMAND hocket_stress --count 5 --token-file "${CMAKE_CURRENT_BINARY_DIR}/ünï ελλ テスト/api.json")
set_tests_properties(hocket_stress_not_running PROPERTIES
    PASS_REGULAR_EXPRESSION "^Hocket isn't running")
