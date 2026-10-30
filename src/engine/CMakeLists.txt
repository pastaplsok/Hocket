# hocket_engine: the sync layer between Hocket's project model and Tracktion
# Engine (SPEC §3.3), and playback.
add_library(hocket_engine STATIC
    EditSync.cpp
    EditSync.h)

target_link_libraries(hocket_engine
    PUBLIC
        hocket_core
        hocket_juce)

hocket_enable_warnings(hocket_engine)
