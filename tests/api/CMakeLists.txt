add_executable(hocket_api_tests
    HandlerTests.cpp
    ProtocolTests.cpp
    ServerTests.cpp
    TextChangesTests.cpp
    TokenFileTests.cpp)

# ixwebsocket for the end-to-end tests' client; hocket_platform for its own tests.
target_link_libraries(hocket_api_tests PRIVATE hocket_api hocket_platform ixwebsocket::ixwebsocket doctest::doctest_with_main)
target_include_directories(hocket_api_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects from the source tree, as checked out.
target_compile_definitions(hocket_api_tests PRIVATE HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

hocket_enable_warnings(hocket_api_tests)

add_test(NAME hocket_api_tests COMMAND hocket_api_tests)
