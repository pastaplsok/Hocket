add_executable(hocket_mcp_tests
    BridgeTests.cpp)

target_link_libraries(hocket_mcp_tests PRIVATE hocket_bridge hocket_api hocket_platform doctest::doctest_with_main)
target_include_directories(hocket_mcp_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects from the source tree, as checked out.
target_compile_definitions(hocket_mcp_tests PRIVATE HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

hocket_enable_warnings(hocket_mcp_tests)

add_test(NAME hocket_mcp_tests COMMAND hocket_mcp_tests)

# hocket-mcp itself, as an MCP client runs it: messages on stdin, replies on
# stdout. Its token file and label are in a folder whose name is outside every
# Western ANSI code page, so this covers argument decoding too.
set(unicode_name "ünï ελλ テスト")

add_test(NAME hocket_mcp_stdio
    COMMAND "${CMAKE_COMMAND}"
        "-DHOCKET_MCP=$<TARGET_FILE:hocket_mcp>"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/${unicode_name}"
        "-DLABEL=${unicode_name}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/StdioTest.cmake")
