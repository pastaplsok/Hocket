# hocket_storage: a project's folder on disk, and saving into it (SPEC §4.5).
# Not in hocket_core, since it needs the platform layer, which links core.
add_library(hocket_storage STATIC
    ProjectFolder.cpp
    ProjectFolder.h)

target_link_libraries(hocket_storage
    PUBLIC
        hocket_core
    PRIVATE
        hocket_platform
        nlohmann_json::nlohmann_json)

hocket_enable_warnings(hocket_storage)
