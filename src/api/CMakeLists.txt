# hocket_api: the WebSocket API that MCP clients reach through hocket-mcp
# (SPEC §6.3), its server and its client. No JUCE: the app gives the server a
# way to run code on its message thread.
add_library(hocket_api STATIC
    Client.cpp
    Client.h
    Handler.cpp
    Handler.h
    Protocol.cpp
    Protocol.h
    Server.cpp
    Server.h
    TextChanges.cpp
    TextChanges.h
    TokenFile.cpp
    TokenFile.h
    Tools.cpp
    Tools.h)

target_link_libraries(hocket_api
    PUBLIC
        hocket_core
        nlohmann_json::nlohmann_json
    PRIVATE
        hocket_platform
        ixwebsocket::ixwebsocket)

# hello compares versions (Protocol.h).
target_compile_definitions(hocket_api PRIVATE HOCKET_VERSION="${PROJECT_VERSION}")

hocket_enable_warnings(hocket_api)
