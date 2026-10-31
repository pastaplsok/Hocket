#include "mcp/ApiClient.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <condition_variable>
#include <mutex>
#include <optional>

namespace hocket::mcp
{
using Json = nlohmann::json;

// What IXWebSocket's thread tells the waiting thread.
struct ApiClient::State
{
    std::mutex mutex;
    std::condition_variable changed;
    bool opened = false;
    bool closed = false;
    std::string error;         // why the connection failed or closed, if IXWebSocket says
    std::int64_t awaiting = 0; // the ID of the request waiting for its response; 0 for none
    std::optional<Json> response;
};

ApiClient::ApiClient()
    : state (std::make_shared<State>())
{
    netSystemStarted = ix::initNetSystem();
}

ApiClient::~ApiClient()
{
    close();

    if (netSystemStarted)
        ix::uninitNetSystem();
}

std::string ApiClient::connect (int port, std::chrono::milliseconds timeout)
{
    close();
    state = std::make_shared<State>();
    socket = std::make_unique<ix::WebSocket>();
    socket->setUrl ("ws://127.0.0.1:" + std::to_string (port));
    socket->disableAutomaticReconnection();
    socket->disablePerMessageDeflate();
    socket->setHandshakeTimeout (static_cast<int> (std::chrono::ceil<std::chrono::seconds> (timeout).count()));

    socket->setOnMessageCallback ([shared = state] (const ix::WebSocketMessagePtr& message)
    {
        std::lock_guard lock (shared->mutex);

        switch (message->type)
        {
            case ix::WebSocketMessageType::Open:
                shared->opened = true;
                break;

            case ix::WebSocketMessageType::Close:
                shared->closed = true;
                shared->error = message->closeInfo.reason;
                break;

            case ix::WebSocketMessageType::Error:
                shared->closed = true;
                shared->error = message->errorInfo.reason;
                break;

            case ix::WebSocketMessageType::Message:
            {
                // One request at a time, so an error about a message the API
                // couldn't read (no ID) answers the one waiting.
                auto json = Json::parse (message->str, nullptr, false);

                if (shared->awaiting == 0 || ! json.is_object() || ! json.contains ("id"))
                    return;

                const auto& id = json.at ("id");

                if ((id.is_number_integer() && id.get<std::int64_t>() == shared->awaiting) || (id.is_null() && json.contains ("error")))
                    shared->response = std::move (json);

                break;
            }

            case ix::WebSocketMessageType::Ping:
            case ix::WebSocketMessageType::Pong:
            case ix::WebSocketMessageType::Fragment:
                return;
        }

        shared->changed.notify_all();
    });

    socket->start();

    std::unique_lock lock (state->mutex);
    const auto settled = state->changed.wait_for (lock, timeout, [this] { return state->opened || state->closed; });
    const auto error = ! settled ? "no answer within " + std::to_string (timeout.count()) + " ms"
                     : state->closed ? (state->error.empty() ? std::string ("the connection was refused") : state->error)
                                     : std::string();
    lock.unlock();

    if (! error.empty())
        close();

    return error;
}

bool ApiClient::isOpen() const
{
    std::lock_guard lock (state->mutex);
    return state->opened && ! state->closed;
}

ApiClient::Response ApiClient::request (const std::string& method, const Json& params, std::chrono::milliseconds timeout)
{
    if (! isOpen())
        return { Status::closed, {} };

    const auto id = nextId++;

    {
        std::lock_guard lock (state->mutex);
        state->awaiting = id;
        state->response.reset();
    }

    // IXWebSocket refuses text that isn't valid UTF-8 (and closes the
    // connection), so replace anything that isn't.
    const Json message { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } };

    if (! socket->sendText (message.dump (-1, ' ', false, Json::error_handler_t::replace)).success)
        return { Status::closed, {} };

    std::unique_lock lock (state->mutex);
    state->changed.wait_for (lock, timeout, [this] { return state->response.has_value() || state->closed; });
    state->awaiting = 0;

    if (state->response)
        return { Status::answered, std::move (*state->response) };

    return { state->closed ? Status::closed : Status::timedOut, {} };
}

void ApiClient::close()
{
    // Joins IXWebSocket's thread, so call it without holding the state's lock.
    if (socket != nullptr)
        socket->stop();

    socket.reset();

    std::lock_guard lock (state->mutex);
    state->closed = true;
}
}
