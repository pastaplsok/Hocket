#pragma once

// The WebSocket API's server (SPEC §6.3): IXWebSocket, on 127.0.0.1 only.
// IXWebSocket runs a thread per connection, and those threads never touch the
// command bus: each message is posted to the message thread, handled there by
// a Handler, and the reply is sent from there.

#include "api/Handler.h"

#include <functional>
#include <memory>
#include <string>

namespace ix
{
    class WebSocketServer;
}

namespace hocket::api
{
    // Runs a function on the message thread, later. It's called from
    // IXWebSocket's threads; hocket-headless passes MessageManager::callAsync.
    using PostToMessageThread = std::function<void (std::function<void()>)>;

    struct ServerOptions
    {
        int firstPort = 46253; // tried first, then the ones after it
        int portsToTry = 10;
        std::function<void (const std::string&)> log; // optional; called on the message thread
    };

    class Server
    {
    public:
        // The handler must outlive the server. Create, start and destroy the
        // server on the message thread.
        Server (Handler& handler, PostToMessageThread post, ServerOptions options = {});
        ~Server();

        Server (const Server&) = delete;
        Server& operator= (const Server&) = delete;

        // Listens on the first free port in the range. Returns an error, or an empty string.
        std::string start();

        // The port it's listening on; 0 until start() succeeds.
        int port() const { return listeningPort; }

    private:
        struct Connections;

        std::shared_ptr<Connections> connections;
        PostToMessageThread post;
        ServerOptions options;
        std::unique_ptr<ix::WebSocketServer> server;
        int listeningPort = 0;
        bool netSystemStarted = false;
    };
}
