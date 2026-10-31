#pragma once

// A client's connection to the running DAW's WebSocket API (SPEC §6.3), as
// blocking calls: send a request, wait for its response. hocket-mcp and
// hocket-stress use it. IXWebSocket runs the connection on a thread of its
// own; use this class from one thread.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ix
{
    class WebSocket;
}

namespace hocket::api
{
    class Client
    {
    public:
        Client();
        ~Client();

        Client (const Client&) = delete;
        Client& operator= (const Client&) = delete;

        // Connects to the API on 127.0.0.1, closing any connection already open.
        // Returns an error, or an empty string.
        std::string connect (int port, std::chrono::milliseconds timeout);

        // False before connect(), and once either side has closed the connection.
        bool isOpen() const;

        enum class Status
        {
            answered,
            closed,  // the connection closed before the response came, or wasn't open
            timedOut // no response in time; if one comes later, it's dropped
        };

        struct Response
        {
            Status status;
            nlohmann::json message; // when answered: the JSON-RPC response, with "result" or "error"
        };

        // Sends a JSON-RPC request and waits for its response.
        Response request (const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout);

        void close();

    private:
        struct State;

        std::shared_ptr<State> state; // shared with the current connection's callbacks
        std::unique_ptr<ix::WebSocket> socket;
        std::int64_t nextId = 1;
        bool netSystemStarted = false;
    };
}
