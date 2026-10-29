# hocket_core: the project model, file formats and command bus. No JUCE, so it
# builds fast and its tests don't need the engine.
add_library(hocket_core STATIC
    notes/Notes.h
    notes/NotesParser.cpp
    notes/NotesSyntax.cpp
    notes/NotesSyntax.h
    notes/NotesWriter.cpp)

target_include_directories(hocket_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(hocket_core PUBLIC cxx_std_20)

hocket_enable_warnings(hocket_core)
