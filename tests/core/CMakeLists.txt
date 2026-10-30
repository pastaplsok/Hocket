add_executable(hocket_core_tests
    CommandBusPropertyTests.cpp
    CommandBusTests.cpp
    JsonTextTests.cpp
    NotesSyntaxTests.cpp
    NotesFormatTests.cpp
    NotesPropertyTests.cpp
    PathsTests.cpp
    ProjectPropertyTests.cpp
    ProjectTests.cpp)

target_link_libraries(hocket_core_tests PRIVATE hocket_core doctest::doctest_with_main)

# Tests load the example projects from the source tree, as checked out.
target_compile_definitions(hocket_core_tests PRIVATE HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

hocket_enable_warnings(hocket_core_tests)

add_test(NAME hocket_core_tests COMMAND hocket_core_tests)
