#pragma once

// The WebSocket API's request handling (SPEC §6.3), without the sockets: one
// JSON-RPC 2.0 message in, the reply out, with the command bus doing the work.
//
// A connection's first request must be hello, with the token from the token
// file, the client's build (its version and fingerprint, Protocol.h), and its
// identity: its name ("Claude"), its session ID, and an optional label.
// Anything else, a wrong token, or a client from another build gets an error
// and the connection is closed. After that it can send tools/list and
// tools/call, shaped like MCP's, so hocket-mcp forwards them unchanged.
//
// When a client's last connection closes, or it sends no requests for the idle
// timeout, its open transaction is committed as "(interrupted)" (SPEC §3.1),
// and its next tool result says so, even from a new connection.
//
// Like the bus, it's used from the message thread only.

#include "api/Protocol.h"
#include "core/commands/CommandBus.h"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::api
{
    using Clock = std::chrono::steady_clock;

    struct HandlerOptions
    {
        std::string token; // clients must send this in hello; never empty
        std::chrono::seconds idleTimeout = defaultIdleTimeout;
        std::string serverName = "Hocket";
        std::function<Clock::time_point()> now = [] { return Clock::now(); };
        std::function<void (const std::string&)> log; // optional: connections, changes, refusals
    };

    struct Reply
    {
        std::string text;   // a JSON-RPC response, or empty when there's nothing to send
        bool close = false; // close the connection once it's sent
    };

    class Handler
    {
    public:
        Handler (commands::CommandBus& bus, HandlerOptions options);

        // Connection IDs are the caller's to choose, unique among open connections.
        void connectionOpened (int connectionId);
        Reply handleMessage (int connectionId, std::string_view message);
        void connectionClosed (int connectionId);

        // Commits the open transaction as "(interrupted)" if its owner is an API
        // client that hasn't sent a request for the idle timeout. Call it regularly.
        void checkIdle();

    private:
        struct Client
        {
            commands::Author author;
            int connections = 0;
            Clock::time_point lastRequest;
            std::vector<std::string> notes; // added to its next tool result
        };

        commands::CommandBus& bus;
        HandlerOptions options;
        std::string fingerprint = buildFingerprint();
        std::map<int, std::optional<commands::Author>> connections; // nullopt until hello
        std::vector<Client> clients;

        Client* findClient (const commands::Author& author);
        void forgetIfDone (const commands::Author& author);
        // `when` starts the note to the client ("when you disconnected"); `reason` is for the log.
        void interrupt (Client& client, const std::string& when, const std::string& reason);
        void log (const std::string& message) const;
    };
}
