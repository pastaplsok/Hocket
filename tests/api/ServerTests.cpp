// End-to-end tests: an IXWebSocket client in this process talks to the server
// on 127.0.0.1, and the test thread stands in for the message thread.

#include "api/Server.h"
#include "core/Paths.h"
#include "core/project/Project.h"
#include "platform/Platform.h"

#include <doctest/doctest.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>

using namespace hocket;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
    const std::string token = "fedcba9876543210fedcba9876543210";

    // Clear of hocket-headless's ports, so one running on this machine doesn't get in the way.
    constexpr int testPort = 46300;

    project::Project demoProject()
    {
        auto loaded = project::load (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo"));
        REQUIRE (loaded.project);
        return std::move (*loaded.project);
    }

    // Stands in for the app's message thread: the test thread runs what's posted to it.
    class MessageThread
    {
    public:
        api::PostToMessageThread poster()
        {
            return [this] (std::function<void()> function)
            {
                std::lock_guard lock (mutex);
                queue.push_back (std::move (function));
                posted.notify_all();
            };
        }

        // Runs what's posted until `done` returns true. False if that takes longer than the timeout.
        bool runUntil (const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            while (! done())
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;

                std::function<void()> next;

                {
                    // `done` may be waiting on another thread, so wake up now and then to ask again.
                    std::unique_lock lock (mutex);

                    if (! posted.wait_for (lock, 10ms, [this] { return ! queue.empty(); }))
                        continue;

                    next = std::move (queue.front());
                    queue.pop_front();
                }

                next();
            }

            return true;
        }

        // Runs everything posted so far.
        void runAll()
        {
            runUntil ([this] { std::lock_guard lock (mutex); return queue.empty(); });
        }

    private:
        std::mutex mutex;
        std::condition_variable posted;
        std::deque<std::function<void()>> queue;
    };

    // A WebSocket client, as hocket-mcp will be.
    class Client
    {
    public:
        explicit Client (int port, const ix::WebSocketHttpHeaders& headers = {})
        {
            socket.setUrl ("ws://127.0.0.1:" + std::to_string (port));
            socket.disableAutomaticReconnection();
            socket.setExtraHeaders (headers);
            socket.setOnMessageCallback ([this] (const ix::WebSocketMessagePtr& message)
            {
                std::lock_guard lock (mutex);

                if (message->type == ix::WebSocketMessageType::Open)
                    opened = true;
                else if (message->type == ix::WebSocketMessageType::Close)
                    closed = true;
                else if (message->type == ix::WebSocketMessageType::Error)
                    closed = true, error = message->errorInfo.reason;
                else if (message->type == ix::WebSocketMessageType::Message)
                    replies.push_back (Json::parse (message->str));
            });
            socket.start();
        }

        ~Client() { socket.stop(); }

        bool isOpen() { std::lock_guard lock (mutex); return opened && ! closed; }
        bool isClosed() { std::lock_guard lock (mutex); return closed; }
        std::string lastError() { std::lock_guard lock (mutex); return error; }
        std::size_t replyCount() { std::lock_guard lock (mutex); return replies.size(); }
        Json reply (std::size_t index) { std::lock_guard lock (mutex); return replies.at (index); }

        void send (const std::string& method, const Json& params)
        {
            socket.sendText (Json { { "jsonrpc", "2.0" }, { "id", nextId++ }, { "method", method }, { "params", params } }.dump());
        }

        void hello (const std::string& withToken = token)
        {
            send ("hello", { { "token", withToken }, { "client", "Claude" }, { "session", "session-1" } });
        }

        void call (const std::string& tool, const Json& arguments)
        {
            send ("tools/call", { { "name", tool }, { "arguments", arguments } });
        }

        void disconnect() { socket.stop(); }

    private:
        ix::WebSocket socket;
        std::mutex mutex;
        bool opened = false;
        bool closed = false;
        std::string error;
        std::vector<Json> replies;
        int nextId = 1;
    };

    struct Fixture
    {
        MessageThread messageThread; // outlives the server, which posts to it until it's stopped
        commands::CommandBus bus { demoProject() };
        std::vector<std::string> logLines;
        api::Handler handler { bus, handlerOptions() };

        api::HandlerOptions handlerOptions()
        {
            api::HandlerOptions options;
            options.token = token;
            options.log = [this] (const std::string& line) { logLines.push_back (line); };
            return options;
        }

        api::ServerOptions serverOptions (int firstPort = testPort, int portsToTry = 10)
        {
            return { firstPort, portsToTry, [this] (const std::string& line) { logLines.push_back (line); } };
        }

        bool logged (std::string_view part) const
        {
            return std::ranges::any_of (logLines, [part] (const std::string& line) { return line.find (part) != std::string::npos; });
        }
    };

    // Connects and says hello.
    void connect (Fixture& fixture, Client& client)
    {
        INFO ("client error: " << client.lastError());
        INFO ("server log: " << fixture.logLines.size() << " lines, last: " << (fixture.logLines.empty() ? "" : fixture.logLines.back()));
        REQUIRE (fixture.messageThread.runUntil ([&client] { return client.isOpen(); }));
        client.hello();
        REQUIRE (fixture.messageThread.runUntil ([&client] { return client.replyCount() == 1; }));
        REQUIRE (client.reply (0).contains ("result"));
    }
}

TEST_CASE ("a client with the token calls tools over a real connection")
{
    Fixture fixture;
    api::Server server (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (server.start().empty());
    CHECK (server.port() == testPort);

    Client client (server.port());
    connect (fixture, client);
    CHECK (client.reply (0).at ("result").at ("author") == "Claude");

    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1      C2    1/4   v100\n" } });
    REQUIRE (fixture.messageThread.runUntil ([&client] { return client.replyCount() == 2; }));
    CHECK (client.reply (1).at ("result").at ("content")[0].at ("text") == "#1 wrote clip clp-bass (1 note)\nStored exactly as you sent it.");
    CHECK (fixture.bus.history().size() == 1);
    CHECK (fixture.logged ("Claude connected"));
}

TEST_CASE ("a wrong token gets an error, then the server closes the connection")
{
    Fixture fixture;
    api::Server server (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (server.start().empty());

    Client client (server.port());
    REQUIRE (fixture.messageThread.runUntil ([&client] { return client.isOpen(); }));
    client.hello ("guess");

    REQUIRE (fixture.messageThread.runUntil ([&client] { return client.isClosed(); }));
    REQUIRE (client.replyCount() == 1);
    CHECK (client.reply (0).at ("error").at ("code") == -32001);
}

TEST_CASE ("connections from web pages are refused before they can say anything")
{
    Fixture fixture;
    api::Server server (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (server.start().empty());

    for (const auto* origin : { "https://example.com", "null", "chrome-extension://abcdefgh" })
    {
        CAPTURE (origin);
        Client client (server.port(), { { "Origin", origin } });
        REQUIRE (fixture.messageThread.runUntil ([&client] { return client.isClosed(); }));
        REQUIRE (fixture.messageThread.runUntil ([&fixture, origin] { return fixture.logged ("refused a connection from a web page (" + std::string (origin) + ")"); }));
        CHECK (client.replyCount() == 0);
    }

    // Native clients send a ws:// origin, as IXWebSocket's does unless told otherwise, or none.
    Client native (server.port(), { { "Origin", "ws://localhost:46300" } });
    connect (fixture, native);
}

TEST_CASE ("when a client disconnects, its transaction is committed as interrupted")
{
    Fixture fixture;
    api::Server server (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (server.start().empty());

    Client client (server.port());
    connect (fixture, client);
    client.call ("begin_transaction", { { "name", "wrote bridge" } });
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });
    REQUIRE (fixture.messageThread.runUntil ([&client] { return client.replyCount() == 3; }));
    REQUIRE (fixture.bus.openTransaction().has_value());

    client.disconnect();
    REQUIRE (fixture.messageThread.runUntil ([&fixture] { return ! fixture.bus.openTransaction().has_value(); }));
    CHECK (fixture.bus.history().front().description == "wrote bridge (interrupted)");
}

TEST_CASE ("a port that's taken is skipped")
{
    Fixture fixture;
    api::Server first (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (first.start().empty());
    CHECK_FALSE (platform::isTcpPortFree (first.port()));

    api::Server second (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (second.start().empty());
    CHECK (second.port() == first.port() + 1);

    api::Server none (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions (first.port(), 2));
    CHECK (none.start() == "couldn't start the API: ports 46300 to 46301 are all in use");
    CHECK (none.port() == 0);
}

TEST_CASE ("a port is free again once its server is gone")
{
    Fixture fixture;

    {
        api::Server server (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
        REQUIRE (server.start().empty());

        // Leave a connection behind, so the port has one in TIME_WAIT.
        Client client (server.port());
        connect (fixture, client);
    }

    CHECK (platform::isTcpPortFree (testPort));
}

TEST_CASE ("messages still waiting for the message thread when the server goes are dropped")
{
    Fixture fixture;
    auto server = std::make_unique<api::Server> (fixture.handler, fixture.messageThread.poster(), fixture.serverOptions());
    REQUIRE (server->start().empty());

    Client client (server->port());
    connect (fixture, client);
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });

    // Wait for the server to receive it, but don't run it.
    std::this_thread::sleep_for (200ms);
    server.reset();
    fixture.messageThread.runAll();

    CHECK (fixture.bus.history().empty());
}
