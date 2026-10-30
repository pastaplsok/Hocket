# Engine tests: the sync layer, offline renders and live playback through a
# fake audio device. They need JUCE's message manager and a Tracktion Engine,
# so they have their own main.
add_executable(hocket_engine_tests
    EditSyncTests.cpp
    EngineTestMain.cpp
    EngineTesting.cpp
    EngineTesting.h
    LivePlaybackTests.cpp
    RenderTests.cpp)

target_link_libraries(hocket_engine_tests PRIVATE hocket_engine doctest::doctest)
target_include_directories(hocket_engine_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects from the source tree, as checked out.
target_compile_definitions(hocket_engine_tests PRIVATE HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

hocket_enable_warnings(hocket_engine_tests)

add_test(NAME hocket_engine_tests COMMAND hocket_engine_tests)
