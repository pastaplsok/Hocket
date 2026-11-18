# hocket_engine: the sync layer between Hocket's project model and Tracktion
# Engine (SPEC §3.3), and playback.
add_library(hocket_engine STATIC
    devices/DrumVoices.cpp
    devices/DrumVoices.h
    devices/PlayableKit.cpp
    devices/PlayableKit.h
    EditSync.cpp
    EditSync.h
    MasterOutput.cpp
    MasterOutput.h
    mix/GainStage.cpp
    mix/GainStage.h
    mix/GainStagePlugin.cpp
    mix/GainStagePlugin.h
    PlaybackDevice.cpp
    PlaybackDevice.h
    kits/KitLibrary.cpp
    kits/KitLibrary.h
    kits/SampleRateConversion.cpp
    kits/SampleRateConversion.h)

target_link_libraries(hocket_engine
    PUBLIC
        hocket_core
        hocket_juce)

hocket_enable_warnings(hocket_engine)
