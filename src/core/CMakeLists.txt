# hocket_core: the project model, file formats and command bus. No JUCE, so it
# builds fast and its tests don't need the engine.
add_library(hocket_core STATIC
    Paths.cpp
    Paths.h
    commands/CommandBus.cpp
    commands/CommandBus.h
    commands/WatchedSaves.cpp
    commands/WatchedSaves.h
    notes/Notes.h
    notes/NotesParser.cpp
    notes/NotesSyntax.cpp
    notes/NotesSyntax.h
    notes/NotesWriter.cpp
    project/ClipFileWatcher.cpp
    project/ClipFileWatcher.h
    project/JsonText.cpp
    project/JsonText.h
    project/Project.cpp
    project/Project.h
    project/ProjectReader.cpp
    project/ProjectWriter.cpp)

target_include_directories(hocket_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(hocket_core PUBLIC cxx_std_20)
target_link_libraries(hocket_core PRIVATE nlohmann_json::nlohmann_json)

hocket_enable_warnings(hocket_core)
