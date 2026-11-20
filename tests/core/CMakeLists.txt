add_executable(hocket_core_tests
    BundledKitTests.cpp
    ClipCommandTests.cpp
    ClipFileWatcherTests.cpp
    CommandBusPropertyTests.cpp
    CommandBusTests.cpp
    CommandSequenceTests.cpp
    DrumMapTests.cpp
    GestureTests.cpp
    JsonTextTests.cpp
    KitTests.cpp
    NotesSyntaxTests.cpp
    NotesFormatTests.cpp
    NotesPropertyTests.cpp
    PathsTests.cpp
    ProjectPropertyTests.cpp
    ProjectTests.cpp
    ReplaceProjectTests.cpp
    RulesTests.cpp
    SaveTrackingTests.cpp
    SongCommandTests.cpp
    TrackCommandTests.cpp
    TransportCommandTests.cpp
    UnitsTests.cpp
    WavHeaderTests.cpp
    WatchedSavesTests.cpp)

# hocket_platform: its FileLock holds a file the way an editor might.
target_link_libraries(hocket_core_tests PRIVATE hocket_core hocket_platform doctest::doctest_with_main)
target_include_directories(hocket_core_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects and the bundled kits from the source tree, as checked out.
target_compile_definitions(hocket_core_tests PRIVATE
    HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
    HOCKET_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    HOCKET_KITS_DIR="${PROJECT_SOURCE_DIR}/assets/kits")

hocket_enable_warnings(hocket_core_tests)

add_test(NAME hocket_core_tests COMMAND hocket_core_tests)
