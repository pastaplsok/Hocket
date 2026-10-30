#include "api/Handler.h"
#include "api/Tools.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <set>

namespace hocket::api
{
namespace
{
    using Json = nlohmann::json;

    // JSON-RPC 2.0 error codes; -32001 is in the range left for servers.
    constexpr int parseError = -32700;
    constexpr int invalidRequest = -32600;
    constexpr int methodNotFound = -32601;
    constexpr int invalidParams = -32602;
    constexpr int notAuthenticated = -32001;

    constexpr std::size_t maxNameLength = 64;
    constexpr std::size_t maxSessionLength = 128;

    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string dump (const Json& json)
    {
        // Clip text read from disk isn't checked for valid UTF-8, so don't throw on it.
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

    std::string firstLine (std::string_view text)
    {
        return std::string (text.substr (0, text.find ('\n')));
    }

    // Compares every byte, so the time it takes doesn't tell how much of a guess was right.
    bool sameToken (std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;

        unsigned char difference = 0;

        for (std::size_t i = 0; i < a.size(); ++i)
            difference |= static_cast<unsigned char> (a[i] ^ b[i]);

        return difference == 0;
    }

    // Names end up in messages, the history and the log.
    bool isPlainText (std::string_view text, std::size_t maxLength)
    {
        return ! text.empty() && text.size() <= maxLength
            && std::ranges::none_of (text, [] (unsigned char c) { return c < 0x20 || c == 0x7f; });
    }

    struct HelloError
    {
        int code = 0;
        std::string message;
    };

    // Reads the client's identity from hello's params into author.
    std::optional<HelloError> readHello (const Json& params, std::string_view token, commands::Author& author)
    {
        static const std::set<std::string> keys { "token", "client", "session", "label" };
        const auto format = std::string (R"(hello's params are {"token": "...", "client": "Claude", "session": "...", "label": "..."}, with label optional)");

        if (! params.is_object())
            return HelloError { invalidParams, format };

        for (const auto& [key, value] : params.items())
            if (! keys.contains (key))
                return HelloError { invalidParams, "hello has no parameter " + singleQuoted (key) + "; " + format };

        if (! params.contains ("token") || ! params.at ("token").is_string())
            return HelloError { invalidParams, "hello needs \"token\": the token from Hocket's token file" };

        if (! sameToken (params.at ("token").get<std::string>(), token))
            return HelloError { notAuthenticated, "the token is wrong. Hocket makes a new one each time it starts, so read the current one from its token file" };

        const auto text = [&params] (const char* key) { return params.contains (key) && params.at (key).is_string() ? params.at (key).get<std::string>() : std::string(); };
        author = { text ("client"), text ("session"), text ("label") };

        if (! isPlainText (author.name, maxNameLength))
            return HelloError { invalidParams, "hello needs \"client\": the client's name as people should see it in the history, e.g. \"Claude\" "
                                               "(1 to 64 bytes of text, no control characters)" };

        if (! isPlainText (author.session, maxSessionLength))
            return HelloError { invalidParams, "hello needs \"session\": an ID that stays the same for the client's whole session, "
                                               "across reconnects (1 to 128 bytes of text, no control characters)" };

        if (params.contains ("label") && ! (params.at ("label").is_string() && (author.label.empty() || isPlainText (author.label, maxNameLength))))
            return HelloError { invalidParams, "hello's \"label\" is optional, and tells two clients with the same name apart, e.g. \"drums\" "
                                               "(up to 64 bytes of text, no control characters)" };

        return {};
    }

    std::string toolNames (std::chrono::seconds idleTimeout)
    {
        std::string names;

        for (const auto& tool : toolDefinitions (idleTimeout))
            names += (names.empty() ? "" : ", ") + tool.at ("name").get<std::string>();

        return names;
    }
}

//==============================================================================
Handler::Handler (commands::CommandBus& commandBus, HandlerOptions handlerOptions)
    : bus (commandBus), options (std::move (handlerOptions))
{
    assert (! options.token.empty());
}

void Handler::connectionOpened (int connectionId)
{
    connections.emplace (connectionId, std::nullopt);
}

Reply Handler::handleMessage (int connectionId, std::string_view message)
{
    const auto connection = connections.find (connectionId);
    assert (connection != connections.end());

    if (connection == connections.end())
        return {};

    const auto helloDone = connection->second.has_value();

    // Before hello, any mistake closes the connection.
    const auto fail = [this, helloDone] (const Json& id, int code, const std::string& text)
    {
        if (! helloDone)
            log ("refused a connection: " + text);

        return Reply { errorReply (id, code, text), ! helloDone };
    };

    Json request;

    try
    {
        request = Json::parse (message);
    }
    catch (const Json::parse_error& error)
    {
        return fail (nullptr, parseError, std::string ("the message isn't valid JSON: ") + error.what());
    }

    if (request.is_array())
        return fail (nullptr, invalidRequest, "batches aren't supported; send one request per message");

    if (! request.is_object() || ! request.contains ("jsonrpc") || request.at ("jsonrpc") != "2.0")
        return fail (nullptr, invalidRequest, R"(a request is a JSON-RPC 2.0 object: {"jsonrpc": "2.0", "id": 1, "method": "...", "params": {...}})");

    const auto hasId = request.contains ("id");
    const auto id = hasId ? request.at ("id") : Json();

    if (! (id.is_string() || id.is_number_integer() || id.is_null()))
        return fail (nullptr, invalidRequest, "a request's \"id\" must be a string or a whole number");

    if (! request.contains ("method") || ! request.at ("method").is_string())
        return fail (id, invalidRequest, "a request needs \"method\": hello, tools/list or tools/call");

    const auto method = request.at ("method").get<std::string>();
    const auto params = request.contains ("params") ? request.at ("params") : Json::object();

    if (! helloDone)
    {
        if (method != "hello")
            return fail (id, notAuthenticated, "send hello with the token first; nothing else is accepted before it");

        commands::Author author;

        if (const auto error = readHello (params, options.token, author))
            return fail (id, error->code, error->message);

        if (! hasId)
        {
            log ("refused a connection: its hello had no id, so it couldn't be answered");
            return { {}, true };
        }

        auto* client = findClient (author);

        if (client == nullptr)
            client = &clients.emplace_back (Client { author });

        client->author.label = author.label;
        client->lastRequest = options.now();
        ++client->connections;
        connection->second = author;

        log (commands::displayName (author) + " connected (session " + author.session + ")");
        return { resultReply (id, { { "server", options.serverName }, { "version", options.serverVersion }, { "author", commands::displayName (author) } }) };
    }

    // Notifications get no reply, and the API has none of its own.
    if (! hasId)
        return {};

    const auto& author = *connection->second;
    auto* client = findClient (author);
    assert (client != nullptr);
    client->lastRequest = options.now();

    if (method == "hello")
        return fail (id, invalidRequest, "this connection has already said hello");

    if (method == "tools/list")
        return { resultReply (id, { { "tools", toolDefinitions (options.idleTimeout) } }) };

    if (method != "tools/call")
        return fail (id, methodNotFound, "there's no method " + singleQuoted (method) + "; the methods are hello, tools/list and tools/call");

    if (! params.is_object() || ! params.contains ("name") || ! params.at ("name").is_string())
        return fail (id, invalidParams, R"(tools/call's params are {"name": "<tool>", "arguments": {...}})");

    const auto name = params.at ("name").get<std::string>();
    const auto arguments = params.contains ("arguments") && ! params.at ("arguments").is_null() ? params.at ("arguments") : Json::object();
    auto result = callTool (bus, author, name, arguments);

    if (! result)
        return fail (id, invalidParams, "there's no tool " + singleQuoted (name) + "; the tools are " + toolNames (options.idleTimeout));

    // Tell the client what happened to its transaction while it wasn't looking.
    for (const auto& note : client->notes)
        result->content.push_back ("Note: " + note);

    client->notes.clear();

    if (! isReadOnlyTool (name) || result->isError)
        log (commands::displayName (author) + ": " + name + (result->isError ? " failed: " : ": ") + firstLine (result->content.front()));

    auto content = Json::array();

    for (const auto& text : result->content)
        content.push_back ({ { "type", "text" }, { "text", text } });

    return { resultReply (id, { { "content", std::move (content) }, { "isError", result->isError } }) };
}

void Handler::connectionClosed (int connectionId)
{
    const auto connection = connections.find (connectionId);

    if (connection == connections.end())
        return;

    const auto author = connection->second;
    connections.erase (connection);

    if (! author)
        return;

    auto* client = findClient (*author);
    assert (client != nullptr);
    --client->connections;
    log (commands::displayName (*author) + " disconnected");

    // A client may have several connections; only the last one leaving ends its transaction.
    if (client->connections == 0)
        interrupt (*client, "when you disconnected", "disconnected");

    forgetIfDone (*author);
}

void Handler::checkIdle()
{
    const auto open = bus.openTransaction();

    if (! open)
        return;

    auto* client = findClient (open->author);

    if (client == nullptr || options.now() - client->lastRequest < options.idleTimeout)
        return;

    const auto timeout = durationText (options.idleTimeout);
    interrupt (*client, "after " + timeout + " with no request from you", "no request for " + timeout);
}

void Handler::interrupt (Client& client, const std::string& when, const std::string& reason)
{
    if (const auto open = bus.openTransaction(); ! open || open->author != client.author)
        return;

    const auto result = bus.interruptTransaction (client.author);
    assert (result.ok);

    // e.g. "after 10 minutes with no request from you, your transaction "wrote bridge" was closed; its edits are #3 ..."
    client.notes.push_back (when + ", your " + result.message);
    log (commands::displayName (client.author) + ": " + result.message + " (" + reason + ")");
}

Handler::Client* Handler::findClient (const commands::Author& author)
{
    const auto client = std::ranges::find (clients, author, &Client::author);
    return client == clients.end() ? nullptr : &*client;
}

void Handler::forgetIfDone (const commands::Author& author)
{
    std::erase_if (clients, [&author] (const Client& client) { return client.author == author && client.connections == 0 && client.notes.empty(); });
}

void Handler::log (const std::string& message) const
{
    if (options.log)
        options.log (message);
}
}
