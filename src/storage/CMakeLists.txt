# hocket_storage: a project's folder on disk, saving into it, and the recovery copy (SPEC §4.5).
# Not in hocket_core, since it needs the platform layer, which links core.
add_library(hocket_storage STATIC
    Formats.cpp
    Formats.h
    ProjectFolder.cpp
    ProjectFolder.h
    ProjectWatcher.cpp
    ProjectWatcher.h
    Recovery.cpp
    Recovery.h
    RecoveryCopy.cpp
    RecoveryCopy.h
    WatchedFolder.cpp
    WatchedFolder.h)

target_link_libraries(hocket_storage
    PUBLIC
        hocket_core
    PRIVATE
        hocket_platform
        nlohmann_json::nlohmann_json)

hocket_enable_warnings(hocket_storage)
