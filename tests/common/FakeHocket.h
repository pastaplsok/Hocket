#pragma once

// A running Hocket, as far as an API client can tell: a command bus, the API's
// handler and server on a thread of their own, and a token file naming this
// process. Clients' calls block until the DAW answers, so the DAW can't share
// the test thread; its thread stands in for hocket-headless's message thread.

#include "api/Handler.h"
#include "api/Server.h"
#include "api/TokenFile.h"
#include "core/project/Project.h"
#include "platform/Platform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hocket::testing
{
    class FakeHocket
    {
    public:
        // Serves the project on the first free port from firstPort, and writes the token file.
        FakeHocket (const std::filesystem::path& tokenFile, int firstPort, project::Project projectToServe)
            : file (tokenFile), project (std::move (projectToServe))
        {
            thread = std::thread ([this, firstPort] { run (firstPort); });

            {
                std::unique_lock lock (mutex);
                changed.wait (lock, [this] { return started; });
            }

            if (! startError.empty())
            {
                thread.join(); // run() has returned
                FAIL (startError);
            }

            info = { port, token, platform::currentProcessId(), platform::currentProgramName() };
            REQUIRE (api::writeTokenFile (file, info).empty());
        }

        ~FakeHocket()
        {
            {
                std::lock_guard lock (mutex);
                stopping = true;
            }

            changed.notify_all();
            thread.join();
            api::removeTokenFile (file, info);
        }

        FakeHocket (const FakeHocket&) = delete;
        FakeHocket& operator= (const FakeHocket&) = delete;

        // While paused, requests wait unanswered, as if the message thread were busy.
        void pause (bool shouldPause)
        {
            {
                std::lock_guard lock (mutex);
                paused = shouldPause;
            }

            changed.notify_all();
        }

        // Whether the handler or server has logged a line containing this.
        bool logged (std::string_view part)
        {
            std::lock_guard lock (mutex);
            return std::ranges::any_of (logLines, [part] (const std::string& line) { return line.find (part) != std::string::npos; });
        }

    private:
        std::filesystem::path file;
        project::Project project;
        const std::string token = api::makeToken();
        api::ApiInfo info;
        std::thread thread;

        std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::function<void()>> queue;
        std::vector<std::string> logLines;
        bool started = false;
        std::string startError;
        int port = 0;
        bool paused = false;
        bool stopping = false;

        void log (const std::string& line)
        {
            std::lock_guard lock (mutex);
            logLines.push_back (line);
        }

        // The message thread: creates, runs and destroys the bus, handler and server.
        void run (int firstPort)
        {
            commands::CommandBus bus (std::move (project));

            api::HandlerOptions handlerOptions;
            handlerOptions.token = token;
            handlerOptions.log = [this] (const std::string& line) { log (line); };
            api::Handler handler (bus, std::move (handlerOptions));

            const auto post = [this] (std::function<void()> function)
            {
                {
                    std::lock_guard lock (mutex);
                    queue.push_back (std::move (function));
                }

                changed.notify_all();
            };

            api::Server server (handler, post, { firstPort, 10, [this] (const std::string& line) { log (line); } });
            const auto error = server.start();

            {
                std::lock_guard lock (mutex);
                started = true;
                startError = error;
                port = server.port();
            }

            changed.notify_all();

            while (error.empty())
            {
                std::function<void()> next;

                {
                    std::unique_lock lock (mutex);
                    changed.wait (lock, [this] { return stopping || (! paused && ! queue.empty()); });

                    if (stopping)
                        break;

                    next = std::move (queue.front());
                    queue.pop_front();
                }

                next();
            }

            // The server goes first, closing its connections; what they post now is never run.
        }
    };
}
