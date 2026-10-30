#include "api/Server.h"

#include "platform/Platform.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <map>

namespace hocket::api
{
namespace
{
    constexpr auto host = "127.0.0.1";
    constexpr int handshakeTimeoutSeconds = 3;
    constexpr int noPings = -1;

    // IXWebSocket's server sends in blocking mode, with no timeout by default,
    // so a client that stopped reading would hang the message thread on its
    // reply. With this, the connection is closed instead.
    constexpr int sendTimeoutSeconds = 2;

    // WebSocket close code 1008: policy violation.
    constexpr std::uint16_t refusedCloseCode = 1008;

    // Any web page can open a WebSocket to 127.0.0.1, and browsers always send
    // the page's origin: "https://example.com", "null" for a local file, or an
    // extension's. A page is never loaded from a ws:// URL, so those come from
    // native clients: IXWebSocket's client sends its own URL as the origin
    // unless told otherwise, and some clients send none.
    bool isFromWebPage (const ix::WebSocketHttpHeaders& headers)
    {
        const auto origin = headers.find ("Origin");
        return origin != headers.end() && ! origin->second.starts_with ("ws://") && ! origin->second.starts_with ("wss://");
    }
}

//==============================================================================
// The message thread's side. Callbacks posted from IXWebSocket's threads reach
// it through a weak_ptr, so any still queued when the server goes do nothing.
struct Server::Connections
{
    Handler& handler;
    std::function<void (const std::string&)> log;
    std::map<int, std::weak_ptr<ix::WebSocket>> sockets;

    void opened (int id, std::weak_ptr<ix::WebSocket> socket)
    {
        sockets.emplace (id, std::move (socket));
        handler.connectionOpened (id);
    }

    void received (int id, const std::string& text)
    {
        const auto connection = sockets.find (id);

        if (connection == sockets.end())
            return;

        const auto reply = handler.handleMessage (id, text);

        if (const auto socket = connection->second.lock())
        {
            if (! reply.text.empty())
                socket->sendText (reply.text);

            if (reply.close)
                socket->close (refusedCloseCode, "refused; the last message says why");
        }
    }

    void closed (int id)
    {
        if (sockets.erase (id) > 0)
            handler.connectionClosed (id);
    }

    void write (const std::string& message) const
    {
        if (log)
            log (message);
    }
};

//==============================================================================
Server::Server (Handler& handler, PostToMessageThread postToMessageThread, ServerOptions serverOptions)
    : connections (std::make_shared<Connections> (Connections { handler, serverOptions.log, {} })),
      post (std::move (postToMessageThread)),
      options (std::move (serverOptions))
{
}

Server::~Server()
{
    // Closes every connection and joins IXWebSocket's threads, so nothing more is posted.
    if (server != nullptr)
        server->stop();

    server.reset();
    connections.reset();

    if (netSystemStarted)
        ix::uninitNetSystem();
}

std::string Server::start()
{
    if (! netSystemStarted)
        netSystemStarted = ix::initNetSystem();

    const auto lastPort = options.firstPort + options.portsToTry - 1;
    std::string lastError;

    for (auto port = options.firstPort; port <= lastPort; ++port)
    {
        // IXWebSocket's own bind can't tell (see platform::isTcpPortFree).
        if (! platform::isTcpPortFree (port))
            continue;

        auto candidate = std::make_unique<ix::WebSocketServer> (port, host, ix::SocketServer::kDefaultTcpBacklog, ix::SocketServer::kDefaultMaxConnections,
                                                                handshakeTimeoutSeconds, ix::SocketServer::kDefaultAddressFamily, noPings,
                                                                sendTimeoutSeconds);
        candidate->disablePerMessageDeflate();

        const std::weak_ptr<Connections> weakConnections = connections;
        const auto send = post;

        candidate->setLogCallback ([weakConnections, send] (ix::LogLevel, const std::string& message)
        {
            send ([weakConnections, message]
            {
                if (const auto shared = weakConnections.lock())
                    shared->write ("WebSocket: " + message);
            });
        });

        candidate->setOnConnectionCallback ([weakConnections, send, nextId = std::make_shared<std::atomic<int>> (1)] (std::weak_ptr<ix::WebSocket> socket,
                                                                                                                    std::shared_ptr<ix::ConnectionState>)
        {
            const auto id = nextId->fetch_add (1);
            const auto webSocket = socket.lock();

            if (webSocket == nullptr)
                return;

            webSocket->setOnMessageCallback ([weakConnections, send, socket, id] (const ix::WebSocketMessagePtr& message)
            {
                switch (message->type)
                {
                    case ix::WebSocketMessageType::Open:
                    {
                        // The token keeps web pages out anyway; this turns them away before
                        // they can try one.
                        if (isFromWebPage (message->openInfo.headers))
                        {
                            if (const auto refused = socket.lock())
                                refused->close (refusedCloseCode, "Hocket's API doesn't accept connections from web pages");

                            send ([weakConnections, page = message->openInfo.headers.at ("Origin")]
                            {
                                if (const auto shared = weakConnections.lock())
                                    shared->write ("refused a connection from a web page (" + page + ")");
                            });

                            return;
                        }

                        send ([weakConnections, socket, id]
                        {
                            if (const auto shared = weakConnections.lock())
                                shared->opened (id, socket);
                        });
                        break;
                    }

                    case ix::WebSocketMessageType::Message:
                        send ([weakConnections, id, text = message->str]
                        {
                            if (const auto shared = weakConnections.lock())
                                shared->received (id, text);
                        });
                        break;

                    case ix::WebSocketMessageType::Close:
                    case ix::WebSocketMessageType::Error:
                        send ([weakConnections, id]
                        {
                            if (const auto shared = weakConnections.lock())
                                shared->closed (id);
                        });
                        break;

                    case ix::WebSocketMessageType::Ping:
                    case ix::WebSocketMessageType::Pong:
                    case ix::WebSocketMessageType::Fragment:
                        break;
                }
            });
        });

        if (const auto [listening, error] = candidate->listen(); ! listening)
        {
            lastError = error;
            continue;
        }

        candidate->start();
        server = std::move (candidate);
        listeningPort = port;
        return {};
    }

    return "couldn't start the API: ports " + std::to_string (options.firstPort) + " to " + std::to_string (lastPort)
         + " are all in use" + (lastError.empty() ? std::string() : " (" + lastError + ")");
}
}
