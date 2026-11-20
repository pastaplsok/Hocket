# Engine tests: the sync layer, offline renders and live playback through a
# fake audio device. They need JUCE's message manager and a Tracktion Engine,
# so they have their own main.
add_executable(hocket_engine_tests
    AllocationCounter.cpp
    AllocationCounter.h
    BundledKitRenderTests.cpp
    DrumSamplerTests.cpp
    DrumTesting.h
    DrumVoicesTests.cpp
    EditSyncTests.cpp
    EngineTestMain.cpp
    EngineTesting.cpp
    EngineTesting.h
    GainStageTests.cpp
    KitLibraryTests.cpp
    LivePlaybackTests.cpp
    LiveRevertTests.cpp
    LiveStructureTests.cpp
    LiveTempoTests.cpp
    LiveTransportTests.cpp
    MasterOutputTests.cpp
    MixTests.cpp
    PlaybackDeviceTests.cpp
    RenderTests.cpp
    SampleRateConversionTests.cpp
    StackSampler.cpp
    StackSampler.h
    StressTests.cpp
    StructureSyncTests.cpp)

target_link_libraries(hocket_engine_tests PRIVATE hocket_engine doctest::doctest)

# StackSampler names frames with dbghelp; the main asks for 1 ms timers with winmm.
if(WIN32)
    target_link_libraries(hocket_engine_tests PRIVATE dbghelp winmm)
endif()
target_include_directories(hocket_engine_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")

# Tests load the example projects, fixtures and bundled kits from the source tree, as checked out.
target_compile_definitions(hocket_engine_tests PRIVATE
    HOCKET_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
    HOCKET_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    HOCKET_KITS_DIR="${PROJECT_SOURCE_DIR}/assets/kits")

hocket_enable_warnings(hocket_engine_tests)

add_test(NAME hocket_engine_tests COMMAND hocket_engine_tests)
