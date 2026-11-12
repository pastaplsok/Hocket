add_executable(hocket_core_tests
    ClipCommandTests.cpp
    ClipFileWatcherTests.cpp
    CommandBusPropertyTests.cpp
    CommandBusTests.cpp
    CommandSequenceTests.cpp
    DrumMapTests.cpp
    GestureTests.cpp
    JsonTextTests.cpp
    NotesSyntaxTests.cpp
    NotesFormatTests.cpp
    NotesPropertyTests.cpp
    PathsTests.cpp
    ProjectPropertyTests.cpp
    ProjectTests.cpp
    RulesTests.cpp
    SongCommandTests.cpp
    TrackCommandTests.cpp
    UnitsTests.cpp
    WatchedSavesTests.cpp)

# hocket_platform: its FileLock holds a file the way an editor might.
target_link_libraries(hocket_core_tests PRIVATE hocket_core hocket_platform doctest::doctest_with_main)
target_include_directories(hocket_core_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects from the source tree, as checked out.
target_compile_definitions(hocket_core_tests PRIVATE
    HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
    HOCKET_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")

hocket_enable_warnings(hocket_core_tests)

add_test(NAME hocket_core_tests COMMAND hocket_core_tests)
