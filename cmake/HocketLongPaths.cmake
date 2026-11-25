# Paths longer than 260 characters for a program that reads or writes project
# folders: on Windows, a manifest saying it's long-path aware, which takes
# effect where Windows' long-path setting is on (platform::longPathsEnabled).
function(hocket_long_paths target)
    if(WIN32)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/LongPathAware.manifest")
    endif()
endfunction()
