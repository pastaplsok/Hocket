add_executable(hocket_core_tests
    NotesSyntaxTests.cpp
    NotesFormatTests.cpp
    NotesPropertyTests.cpp)

target_link_libraries(hocket_core_tests PRIVATE hocket_core doctest::doctest_with_main)

hocket_enable_warnings(hocket_core_tests)

add_test(NAME hocket_core_tests COMMAND hocket_core_tests)
