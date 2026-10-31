# hocket-mcp: the MCP server that MCP clients start over stdio. It serves
# Hocket's tool list itself and forwards each tool call to the running DAW's
# WebSocket API (SPEC §6.3). No JUCE.
add_library(hocket_bridge STATIC
    Bridge.cpp
    Bridge.h)

target_link_libraries(hocket_bridge
    PUBLIC
        hocket_api
        nlohmann_json::nlohmann_json
    PRIVATE
        hocket_platform)

hocket_enable_warnings(hocket_bridge)

add_executable(hocket_mcp Main.cpp)
set_target_properties(hocket_mcp PROPERTIES OUTPUT_NAME "hocket-mcp")
target_link_libraries(hocket_mcp PRIVATE hocket_bridge hocket_api hocket_platform)
hocket_enable_warnings(hocket_mcp)

# MCP clients run an installed copy, since Windows won't let the build replace
# a hocket-mcp.exe that's running (docs/connecting-claude-code.md).
if(WIN32)
    # Nor will it let the install replace one, but it will rename one. So delete
    # the copies nothing runs, and move the one a client still runs out of the
    # way; the client picks up the new copy when it restarts hocket-mcp.
    install(CODE [[
        set(bin "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin")
        file(GLOB copies "${bin}/hocket-mcp.exe" "${bin}/hocket-mcp.old-*.exe")

        foreach(file IN LISTS copies)
            execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${file}" RESULT_VARIABLE ignored OUTPUT_QUIET ERROR_QUIET)
        endforeach()

        if(EXISTS "${bin}/hocket-mcp.exe")
            string(RANDOM LENGTH 8 suffix)
            file(RENAME "${bin}/hocket-mcp.exe" "${bin}/hocket-mcp.old-${suffix}.exe")
            message(STATUS "Moved aside: ${bin}/hocket-mcp.exe, which is running")
        endif()
    ]] COMPONENT mcp)
endif()

install(TARGETS hocket_mcp RUNTIME DESTINATION bin COMPONENT mcp)
