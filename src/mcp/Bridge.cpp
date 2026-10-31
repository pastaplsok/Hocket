#include "mcp/Bridge.h"

#include "api/TokenFile.h"
#include "api/Tools.h"
#include "core/Paths.h"
#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <cassert>

namespace hocket::mcp
{
namespace
{
    using Json = nlohmann::json;

    // JSON-RPC 2.0 error codes.
    constexpr int parseError = -32700;
    constexpr int invalidRequest = -32600;
    constexpr int methodNotFound = -32601;
    constexpr int invalidParams = -32602;

    constexpr std::size_t maxNameLength = 64; // hello's limit for names and labels

    // Newest first. A client asking for one of these gets it; any other gets the newest.
    constexpr std::array<std::string_view, 4> protocolVersions { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };

    const std::string notRunning = "Hocket isn't running: start it with `hocket-headless --play <project folder>`, then try again.";

    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string dump (const Json& json)
    {
        // Tool results carry clip text read from disk, which isn't checked for valid UTF-8.
        return json.dump (-1, ' ', false, Json::error_handler_t::replace);
    }

    std::string resultReply (const Json& id, Json result)
    {
        return dump ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move (result) } });
    }

    std::string errorReply (const Json& id, int code, const std::string& message)
    {
        return dump ({ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } });
    }

    // A tool result the model reads as a failure (MCP's isError).
    Json toolError (const std::string& text)
    {
        return { { "content", { { { "type", "text" }, { "text", text } } } }, { "isError", true } };
    }

    std::string seconds (std::chrono::milliseconds duration)
    {
        return api::durationText (std::chrono::ceil<std::chrono::seconds> (duration));
    }

    std::string errorMessage (const Json& response)
    {
        const auto& error = response.at ("error");
        return error.is_object() && error.contains ("message") && error.at ("message").is_string() ? error.at ("message").get<std::string>()
                                                                                                    : dump (error);
    }
}

//==============================================================================
Bridge::Bridge (BridgeOptions bridgeOptions)
    : options (std::move (bridgeOptions)),
      tools (api::toolDefinitions (api::defaultIdleTimeout))
{
    assert (! options.session.empty());
}

std::string Bridge::handleMessage (std::string_view message)
{
    Json request;

    try
    {
        request = Json::parse (message);
    }
    catch (const Json::parse_error& error)
    {
        return errorReply (nullptr, parseError, std::string ("the message isn't valid JSON: ") + error.what());
    }

    if (request.is_array())
        return errorReply (nullptr, invalidRequest, "batches aren't supported; send one request per message");

    if (! request.is_object() || ! request.contains ("jsonrpc") || request.at ("jsonrpc") != "2.0")
        return errorReply (nullptr, invalidRequest, R"(a request is a JSON-RPC 2.0 object: {"jsonrpc": "2.0", "id": 1, "method": "...", "params": {...}})");

    // A response to a request of ours (there are none), or a notification:
    // notifications/initialized and notifications/cancelled need nothing.
    if (! request.contains ("method") || ! request.contains ("id"))
        return {};

    const auto& id = request.at ("id");

    if (! (id.is_string() || id.is_number_integer()))
        return errorReply (nullptr, invalidRequest, "a request's \"id\" must be a string or a whole number");

    if (! request.at ("method").is_string())
        return errorReply (id, invalidRequest, "a request's \"method\" must be a string");

    const auto method = request.at ("method").get<std::string>();
    const auto params = request.contains ("params") ? request.at ("params") : Json::object();

    if (method == "initialize")
        return resultReply (id, initialize (params));

    if (method == "ping")
        return resultReply (id, Json::object());

    if (method == "tools/list")
        return resultReply (id, { { "tools", tools } });

    if (method == "tools/call")
        return callTool (id, params);

    return errorReply (id, methodNotFound, "hocket-mcp has no method " + singleQuoted (method) + "; it serves initialize, ping, tools/list and tools/call");
}

Json Bridge::initialize (const Json& params)
{
    auto requested = std::string();
    auto clientVersion = std::string();

    if (params.is_object())
    {
        if (params.contains ("protocolVersion") && params.at ("protocolVersion").is_string())
            requested = params.at ("protocolVersion").get<std::string>();

        if (params.contains ("clientInfo") && params.at ("clientInfo").is_object())
        {
            const auto& info = params.at ("clientInfo");

            if (info.contains ("name") && info.at ("name").is_string())
                clientName = info.at ("name").get<std::string>();

            if (info.contains ("version") && info.at ("version").is_string())
                clientVersion = info.at ("version").get<std::string>();
        }
    }

    const auto known = std::ranges::find (protocolVersions, requested) != protocolVersions.end();
    const auto version = known ? requested : std::string (protocolVersions.front());

    log ("client " + singleQuoted (clientName) + " " + clientVersion + ", MCP " + requested
         + (known ? "" : " (unknown; offered " + version + ")"));

    return {
        { "protocolVersion", version },
        { "capabilities", { { "tools", { { "listChanged", false } } } } },
        { "serverInfo", { { "name", "hocket-mcp" }, { "title", "Hocket" }, { "version", options.version } } },
        { "instructions", "Hocket is a DAW (music production app) that you and the user work in together. These tools work on the "
                          "project open in the running Hocket; start with get_project_overview. Every change goes into one history "
                          "that the user shares, tagged with who made it." },
    };
}

std::string Bridge::callTool (const Json& id, const Json& params)
{
    if (! params.is_object() || ! params.contains ("name") || ! params.at ("name").is_string())
        return errorReply (id, invalidParams, R"(tools/call's params are {"name": "<tool>", "arguments": {...}})");

    const auto name = params.at ("name").get<std::string>();

    // Checked here too, so a misspelled tool isn't reported as Hocket not running.
    if (std::ranges::none_of (tools, [&name] (const Json& tool) { return tool.at ("name") == name; }))
    {
        std::string names;

        for (const auto& tool : tools)
            names += (names.empty() ? "" : ", ") + tool.at ("name").get<std::string>();

        return errorReply (id, invalidParams, "there's no tool " + singleQuoted (name) + "; the tools are " + names);
    }

    if (const auto error = connect(); ! error.empty())
        return resultReply (id, toolError (error));

    Json call { { "name", name } };

    if (params.contains ("arguments"))
        call["arguments"] = params.at ("arguments");

    const auto response = api.request ("tools/call", call, options.replyTimeout);
    const auto changes = api::isReadOnlyTool (name) ? std::string()
                                                    : " It may or may not have carried out the call, so check with history before trying again.";

    if (response.status == ApiClient::Status::closed)
    {
        log ("the connection closed during " + name);
        return resultReply (id, toolError ("Hocket closed the connection before answering; it may have stopped or restarted." + changes));
    }

    if (response.status == ApiClient::Status::timedOut)
    {
        log (name + " got no answer within " + seconds (options.replyTimeout));
        return resultReply (id, toolError ("Hocket didn't answer within " + seconds (options.replyTimeout) + "." + changes));
    }

    if (response.message.contains ("error"))
    {
        const auto& error = response.message.at ("error");
        const auto code = error.is_object() && error.contains ("code") && error.at ("code").is_number_integer() ? error.at ("code").get<int>() : invalidRequest;
        return errorReply (id, code, errorMessage (response.message));
    }

    auto result = response.message.contains ("result") ? response.message.at ("result") : Json::object();

    // Tell the model, once, that what it knew about the project may be out of date.
    if (restarted && result.is_object() && result.contains ("content") && result.at ("content").is_array())
    {
        result["content"].push_back ({ { "type", "text" },
                                       { "text", "Note: Hocket has restarted since your last call, so the history has started over and the "
                                                 "project may have changed (edits made before the restart may be gone). Call "
                                                 "get_project_overview before relying on what you saw earlier." } });
        restarted = false;
    }

    return resultReply (id, std::move (result));
}

std::string Bridge::connect()
{
    // Read on every call: a restarted DAW has a new port and token, and the
    // connection to one that has just stopped may not have noticed yet.
    const auto info = api::runningInstance (options.tokenFile);

    if (info && info->token == lastToken && api.isOpen())
        return {};

    api.close();

    if (! info)
    {
        const auto stale = api::readTokenFile (options.tokenFile);
        log (stale ? "the token file names " + stale->program + " (process " + std::to_string (stale->processId) + "), which isn't running"
                   : "no token file at " + utf8FromPath (options.tokenFile));
        return notRunning;
    }

    const auto where = "port " + std::to_string (info->port);

    if (const auto error = api.connect (info->port, options.connectTimeout); ! error.empty())
    {
        log ("couldn't connect to " + info->program + " on " + where + ": " + error);
        return notRunning;
    }

    Json hello { { "token", info->token },
                 { "version", options.version },
                 { "fingerprint", options.fingerprint },
                 { "client", fitName (options.name.empty() ? authorName (clientName) : options.name) },
                 { "session", options.session } };

    if (const auto label = fitName (options.label); ! label.empty())
        hello["label"] = label;

    const auto response = api.request ("hello", hello, options.replyTimeout);

    if (response.status != ApiClient::Status::answered)
    {
        api.close();
        log (info->program + " on " + where + " didn't answer hello");
        return "Hocket is running but didn't answer within " + seconds (options.replyTimeout) + "; try again, or restart Hocket if this keeps happening.";
    }

    if (response.message.contains ("error"))
    {
        // Hocket closes the connection itself; this stops IXWebSocket's thread.
        api.close();
        const auto message = errorMessage (response.message);
        log (info->program + " on " + where + " refused hello: " + message);

        // A Hocket from before a change to hello refuses the new params.
        const auto& error = response.message.at ("error");
        const auto mismatch = error.is_object() && error.contains ("code") && error.at ("code") == invalidParams;

        return "Hocket refused hocket-mcp's connection: " + message
             + (mismatch ? " hocket-mcp and Hocket may come from different builds: rebuild both from the same source, then restart the MCP client." : "");
    }

    restarted = restarted || (! lastToken.empty() && lastToken != info->token);
    lastToken = info->token;

    const auto& result = response.message.contains ("result") ? response.message.at ("result") : Json::object();
    log ("connected to " + info->program + " on " + where + " as " + (result.contains ("author") ? dump (result.at ("author")) : hello.at ("client").dump()));
    return {};
}

void Bridge::log (const std::string& message) const
{
    if (options.log)
        options.log (message);
}

//==============================================================================
std::string authorName (std::string_view clientName)
{
    if (clientName == "claude-code")
        return "Claude";

    const auto fitted = fitName (clientName);
    return fitted.empty() ? "MCP client" : fitted;
}

std::string fitName (std::string_view text)
{
    std::string fitted;

    for (const auto c : text)
        if (static_cast<unsigned char> (c) >= 0x20 && c != 0x7f)
            fitted += c;

    if (fitted.size() > maxNameLength)
    {
        // Back up to the start of the character that would be cut.
        auto end = maxNameLength;

        while (end > 0 && (static_cast<unsigned char> (fitted[end]) & 0xc0) == 0x80)
            --end;

        fitted.resize (end);
    }

    return fitted;
}

std::string workingFolderName()
{
    std::error_code error;
    const auto folder = std::filesystem::current_path (error);
    return error ? std::string() : fitName (utf8FromPath (folder.filename()));
}

std::string makeSessionId()
{
    std::array<std::uint8_t, 16> bytes {};

    if (! platform::secureRandomBytes (bytes))
        return {};

    constexpr std::string_view digits = "0123456789abcdef";
    std::string id;

    for (const auto byte : bytes)
    {
        id += digits[byte >> 4];
        id += digits[byte & 0xf];
    }

    return id;
}
}
