// The bridge against a real API server, running as hocket-headless runs it
// (FakeHocket).

#include "api/Protocol.h"
#include "api/TokenFile.h"
#include "api/Tools.h"
#include "common/FakeHocket.h"
#include "common/TemporaryFolder.h"
#include "core/Paths.h"
#include "core/project/Project.h"
#include "mcp/Bridge.h"
#include "platform/Platform.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace hocket;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
    // Clear of hocket-headless's ports and the API tests', so neither gets in the way.
    constexpr int testPort = 46320;

    const std::string notRunning = "Hocket isn't running: start it with `hocket-headless --play <project folder>`, then try again.";

    project::Project demoProject()
    {
        auto loaded = project::load (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo"));
        REQUIRE (loaded.project);
        return std::move (*loaded.project);
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    struct Fixture
    {
        testing::TemporaryFolder folder { testing::unicodeName };
        std::filesystem::path tokenFile = folder.path / pathFromUtf8 ("api.json");
        std::vector<std::string> bridgeLog;

        mcp::BridgeOptions options()
        {
            mcp::BridgeOptions bridgeOptions;
            bridgeOptions.tokenFile = tokenFile;
            bridgeOptions.label = "drums";
            bridgeOptions.session = "test-session";
            bridgeOptions.connectTimeout = 1s;
            bridgeOptions.log = [this] (const std::string& line) { bridgeLog.push_back (line); };
            return bridgeOptions;
        }

        bool bridgeLogged (std::string_view part) const
        {
            return std::ranges::any_of (bridgeLog, [part] (const std::string& line) { return contains (line, part); });
        }
    };

    // An MCP client, driving the bridge by hand.
    struct Client
    {
        mcp::Bridge& bridge;
        int nextId = 1;

        Json request (const std::string& method, const Json& params = Json::object())
        {
            const auto reply = bridge.handleMessage (Json { { "jsonrpc", "2.0" }, { "id", nextId++ }, { "method", method }, { "params", params } }.dump());
            REQUIRE_FALSE (reply.empty());
            return Json::parse (reply);
        }

        Json initialize (const std::string& clientName = "claude-code", const std::string& protocolVersion = "2025-11-25")
        {
            const auto response = request ("initialize", { { "protocolVersion", protocolVersion },
                                                           { "capabilities", Json::object() },
                                                           { "clientInfo", { { "name", clientName }, { "version", "1.0" } } } });
            REQUIRE_MESSAGE (response.contains ("result"), response.dump());
            return response.at ("result");
        }

        // A tool's result: { "content": [...], "isError": ... }
        Json call (const std::string& tool, const Json& arguments = Json::object())
        {
            const auto response = request ("tools/call", { { "name", tool }, { "arguments", arguments } });
            REQUIRE_MESSAGE (response.contains ("result"), response.dump());
            return response.at ("result");
        }

        // All of a tool result's text blocks, joined with blank lines.
        std::string text (const std::string& tool, const Json& arguments = Json::object())
        {
            const auto result = call (tool, arguments);
            std::string joined;

            for (const auto& block : result.at ("content"))
                joined += (joined.empty() ? "" : "\n\n") + block.at ("text").get<std::string>();

            return joined;
        }
    };

    const Json oneNote { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1      C2    1/4   v100\n" } };
}

//==============================================================================
TEST_CASE ("the bridge lists Hocket's tools without Hocket running")
{
    Fixture fixture;
    mcp::Bridge bridge (fixture.options());
    Client client { bridge };

    const auto result = client.initialize();
    CHECK (result.at ("protocolVersion") == "2025-11-25");
    CHECK (result.at ("serverInfo").at ("name") == "hocket-mcp");
    CHECK (result.at ("serverInfo").at ("version") == api::hocketVersion());
    CHECK (result.at ("capabilities").contains ("tools"));

    CHECK (client.request ("tools/list").at ("result").at ("tools") == api::toolDefinitions (api::defaultIdleTimeout));
    CHECK (client.request ("ping").at ("result") == Json::object());
    CHECK (fixture.bridgeLog == std::vector<std::string> { "client 'claude-code' 1.0, MCP 2025-11-25" });
}

TEST_CASE ("the bridge agrees on the client's MCP version if it knows it, and offers its newest if not")
{
    Fixture fixture;
    mcp::Bridge bridge (fixture.options());
    Client client { bridge };

    CHECK (client.initialize ("claude-code", "2024-11-05").at ("protocolVersion") == "2024-11-05");
    CHECK (client.initialize ("claude-code", "2099-01-01").at ("protocolVersion") == "2025-11-25");
}

TEST_CASE ("a call while Hocket isn't running says how to start it")
{
    Fixture fixture;
    mcp::Bridge bridge (fixture.options());
    Client client { bridge };
    client.initialize();

    SUBCASE ("no token file")
    {
        CHECK (client.call ("history") == Json { { "content", { { { "type", "text" }, { "text", notRunning } } } }, { "isError", true } });
        CHECK (fixture.bridgeLogged ("no token file at "));
    }

    SUBCASE ("a token file left behind by a crash")
    {
        REQUIRE (api::writeTokenFile (fixture.tokenFile, { testPort, api::makeToken(), platform::currentProcessId(), "crashed-hocket.exe" }).empty());
        CHECK (client.text ("history") == notRunning);
        CHECK (fixture.bridgeLogged ("the token file names crashed-hocket.exe (process "));
    }

    SUBCASE ("nothing listening on the port")
    {
        REQUIRE (api::writeTokenFile (fixture.tokenFile, { testPort, api::makeToken(), platform::currentProcessId(), platform::currentProgramName() }).empty());
        CHECK (client.text ("history") == notRunning);
        CHECK (fixture.bridgeLogged ("couldn't connect to "));
    }
}

TEST_CASE ("tool calls reach the running Hocket, where the client is the author of its changes")
{
    Fixture fixture;
    testing::FakeHocket hocket (fixture.tokenFile, testPort, demoProject());
    mcp::Bridge bridge (fixture.options());
    Client client { bridge };
    client.initialize();

    const auto original = client.call ("read_clip", { { "clip_id", "clp-bass" } }).at ("content")[1].at ("text").get<std::string>();
    CHECK (hocket.logged ("Claude (drums) connected (session test-session)"));

    CHECK (client.text ("write_clip", oneNote) == "#1 wrote clip clp-bass (1 note)\nStored exactly as you sent it.");
    CHECK (client.text ("history") == "The only entry:\n#1 you: wrote clip clp-bass (1 note)");

    CHECK (contains (client.text ("undo"), "undid #1 \"wrote clip clp-bass (1 note)\" by Claude (drums)"));
    CHECK (client.call ("read_clip", { { "clip_id", "clp-bass" } }).at ("content")[1].at ("text") == original);

    // One connection for all of it.
    CHECK (std::ranges::count_if (fixture.bridgeLog, [] (const std::string& line) { return contains (line, "connected to "); }) == 1);
}

TEST_CASE ("the client's name in Hocket comes from initialize, unless the bridge is given one")
{
    Fixture fixture;
    testing::FakeHocket hocket (fixture.tokenFile, testPort, demoProject());

    SUBCASE ("another client's own name")
    {
        mcp::Bridge bridge (fixture.options());
        Client client { bridge };
        client.initialize ("some-agent");
        client.call ("history");
        CHECK (hocket.logged ("some-agent (drums) connected"));
    }

    SUBCASE ("--name")
    {
        auto options = fixture.options();
        options.name = "Nova";
        options.label = "";
        mcp::Bridge bridge (std::move (options));
        Client client { bridge };
        client.initialize();
        client.call ("history");
        CHECK (hocket.logged ("Nova connected"));
    }
}

TEST_CASE ("the bridge finds Hocket again after it restarts, keeping its session")
{
    Fixture fixture;
    mcp::Bridge bridge (fixture.options());
    Client client { bridge };
    client.initialize();

    std::optional<testing::FakeHocket> hocket;
    hocket.emplace (fixture.tokenFile, testPort, demoProject());
    CHECK (contains (client.text ("write_clip", oneNote), "#1 wrote clip"));

    hocket.reset();
    CHECK (client.text ("history") == notRunning);

    hocket.emplace (fixture.tokenFile, testPort, demoProject());
    const auto history = client.text ("history");
    CHECK (contains (history, "The history is empty"));
    CHECK (contains (history, "Note: Hocket has restarted since your last call"));
    CHECK (hocket->logged ("Claude (drums) connected (session test-session)"));

    // Said once.
    CHECK_FALSE (contains (client.text ("history"), "restarted"));
}

TEST_CASE ("a Hocket from another build turns the bridge away, saying what to do")
{
    Fixture fixture;
    testing::FakeHocket hocket (fixture.tokenFile, testPort, demoProject());
    auto options = fixture.options();
    options.fingerprint = "0123456789abcdef";
    mcp::Bridge bridge (std::move (options));
    Client client { bridge };
    client.initialize();

    const auto result = client.call ("history");
    CHECK (result.at ("isError") == true);
    CHECK (result.at ("content")[0].at ("text") == "Hocket refused hocket-mcp's connection: hocket-mcp and this Hocket come from different builds of version "
                                                     + std::string (api::hocketVersion()) + ": their tools or protocol differ. Rebuild hocket-mcp from the "
                                                     "same source as Hocket and reinstall it, then restart the MCP client so it starts the new hocket-mcp.");
    CHECK (hocket.logged ("refused a connection: hocket-mcp and this Hocket come from different builds"));

    // It tries again on the next call, and says the same.
    CHECK (contains (client.text ("history"), "different builds"));
}

TEST_CASE ("a call Hocket doesn't answer in time is an error, and its late answer is dropped")
{
    Fixture fixture;
    testing::FakeHocket hocket (fixture.tokenFile, testPort, demoProject());
    auto options = fixture.options();
    options.replyTimeout = 300ms;
    mcp::Bridge bridge (std::move (options));
    Client client { bridge };
    client.initialize();
    client.call ("history"); // connected

    hocket.pause (true);
    const auto result = client.call ("write_clip", oneNote);
    CHECK (result.at ("isError") == true);
    CHECK (result.at ("content")[0].at ("text") == "Hocket didn't answer within 1 second. It may or may not have carried out the call, "
                                                     "so check with history before trying again.");

    // The write is still carried out once Hocket gets to it; the next call gets its own answer.
    hocket.pause (false);
    CHECK (client.text ("history") == "The only entry:\n#1 you: wrote clip clp-bass (1 note)");
}

TEST_CASE ("protocol mistakes get JSON-RPC errors, and notifications get no reply")
{
    Fixture fixture;
    mcp::Bridge bridge (fixture.options());
    Client client { bridge };

    const auto errorCode = [&bridge] (std::string_view message)
    {
        const auto reply = Json::parse (bridge.handleMessage (message));
        REQUIRE_MESSAGE (reply.contains ("error"), reply.dump());
        return reply.at ("error").at ("code").get<int>();
    };

    CHECK (errorCode ("{") == -32700);
    CHECK (errorCode ("[]") == -32600);
    CHECK (errorCode (R"({"id": 1, "method": "ping"})") == -32600);
    CHECK (errorCode (R"({"jsonrpc": "2.0", "id": null, "method": "ping"})") == -32600);

    CHECK (bridge.handleMessage (R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").empty());
    CHECK (bridge.handleMessage (R"({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 3}})").empty());
    CHECK (bridge.handleMessage (R"({"jsonrpc": "2.0", "id": 9, "result": {}})").empty());

    const auto unknownMethod = client.request ("resources/list");
    CHECK (unknownMethod.at ("error").at ("code") == -32601);
    CHECK (unknownMethod.at ("error").at ("message") == "hocket-mcp has no method 'resources/list'; it serves initialize, ping, tools/list and tools/call");

    CHECK (client.request ("tools/call", { { "arguments", Json::object() } }).at ("error").at ("code") == -32602);

    // A misspelled tool is reported as such, not as Hocket not running.
    const auto unknownTool = client.request ("tools/call", { { "name", "delete_everything" } });
    CHECK (unknownTool.at ("error").at ("code") == -32602);
    CHECK (contains (unknownTool.at ("error").at ("message").get<std::string>(), "there's no tool 'delete_everything'; the tools are get_project_overview, "));
    CHECK (fixture.bridgeLog.empty());
}

TEST_CASE ("names and labels are made to fit hello")
{
    CHECK (mcp::authorName ("claude-code") == "Claude");
    CHECK (mcp::authorName ("some-agent") == "some-agent");
    CHECK (mcp::authorName ("") == "MCP client");
    CHECK (mcp::authorName ("\t\n") == "MCP client");

    CHECK (mcp::fitName ("a\tb\nc\x7f") == "abc");
    CHECK (mcp::fitName (std::string (70, 'x')) == std::string (64, 'x'));

    // Cut at a character boundary: "テ" is 3 bytes, and only 2 fit after 62.
    CHECK (mcp::fitName (std::string (62, 'x') + "テスト") == std::string (62, 'x'));
    CHECK (mcp::fitName (std::string (61, 'x') + "テスト") == std::string (61, 'x') + "テ");
}
