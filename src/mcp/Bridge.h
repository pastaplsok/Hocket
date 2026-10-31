#pragma once

// hocket-mcp's MCP server (SPEC §6.3), without stdio: one JSON-RPC message
// from the MCP client in, the reply out.
//
// It answers initialize and tools/list itself, from the tool definitions
// compiled into hocket_api, so the client sees Hocket's tools whether or not
// Hocket is running. Each tools/call goes to the running DAW's WebSocket API,
// found through the token file: the bridge connects on the first call, and
// again after the connection closes, reading the file each time so a
// restarted DAW is found. Every connection says hello with the same session
// ID, so the client stays the author of its own history entries.

#include "api/Protocol.h"
#include "mcp/ApiClient.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace hocket::mcp
{
    struct BridgeOptions
    {
        std::filesystem::path tokenFile;
        std::string name;    // the client's name in Hocket's history; empty: from initialize's clientInfo
        std::string label;   // optional; tells two clients with the same name apart, e.g. "drums"
        std::string session; // the same for the bridge's whole run (SPEC §3.1); never empty
        std::string version = std::string (api::hocketVersion());
        std::string fingerprint = api::buildFingerprint();
        std::chrono::milliseconds connectTimeout = std::chrono::seconds (5);
        std::chrono::milliseconds replyTimeout = std::chrono::seconds (30);
        std::function<void (const std::string&)> log; // optional: connections and failures
    };

    class Bridge
    {
    public:
        explicit Bridge (BridgeOptions options);

        // A message from the MCP client. Returns the reply, or an empty string
        // when there's none (notifications, and responses to the client's own requests).
        std::string handleMessage (std::string_view message);

    private:
        BridgeOptions options;
        nlohmann::json tools;
        std::string clientName; // from initialize's clientInfo
        ApiClient api;
        std::string lastToken; // of the DAW last connected to; a new one means it restarted
        bool restarted = false; // not yet told to the model

        nlohmann::json initialize (const nlohmann::json& params);
        std::string callTool (const nlohmann::json& id, const nlohmann::json& params);
        std::string connect(); // returns an error for the model, or an empty string
        void log (const std::string& message) const;
    };

    // How the client appears in Hocket's history: "Claude" for Claude Code,
    // otherwise its name as given, made to fit hello.
    std::string authorName (std::string_view clientName);

    // The text without control characters, cut to hello's 64 bytes at a character boundary.
    std::string fitName (std::string_view text);

    // The working folder's name, for the label; empty for a drive root.
    std::string workingFolderName();

    // 16 bytes from the OS's secure random source, as 32 hex digits; empty if
    // the OS can't supply them.
    std::string makeSessionId();
}
