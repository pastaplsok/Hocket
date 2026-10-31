#include "api/Handler.h"
#include "api/Protocol.h"
#include "core/Paths.h"
#include "core/project/Project.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <set>
#include <stdexcept>

using namespace hocket;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
    const std::string token = "0123456789abcdef0123456789abcdef";

    project::Project demoProject()
    {
        auto loaded = project::load (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo"));
        REQUIRE (loaded.project);
        return std::move (*loaded.project);
    }

    // hello's params for this build, with the token.
    Json helloParams (const std::string& session)
    {
        return { { "token", token }, { "version", api::hocketVersion() }, { "fingerprint", api::buildFingerprint() },
                 { "client", "Claude" }, { "session", session } };
    }

    std::string demoClipText()
    {
        const auto text = readTextFile (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo/clips/bass.notes"));
        REQUIRE (text);
        return *text;
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    // One client connection, driven by hand.
    struct Connection
    {
        api::Handler& handler;
        int id;
        bool closed = false;
        int nextRequestId = 1;

        api::Reply send (const std::string& text)
        {
            auto reply = handler.handleMessage (id, text);
            closed = closed || reply.close;
            return reply;
        }

        // Sends a request and returns the parsed response.
        Json request (const std::string& method, const Json& params = Json::object())
        {
            const auto reply = send (Json { { "jsonrpc", "2.0" }, { "id", nextRequestId++ }, { "method", method }, { "params", params } }.dump());
            REQUIRE_FALSE (reply.text.empty());
            return Json::parse (reply.text);
        }

        Json hello (const std::string& session = "session-1", const std::string& label = {})
        {
            auto params = helloParams (session);

            if (! label.empty())
                params["label"] = label;

            return request ("hello", params);
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

        void close() { handler.connectionClosed (id); }
    };

    struct Fixture
    {
        commands::CommandBus bus { demoProject() };
        api::Clock::time_point now {};
        std::vector<std::string> logLines;
        api::Handler handler { bus, options() };
        int nextConnectionId = 1;

        api::HandlerOptions options()
        {
            api::HandlerOptions handlerOptions;
            handlerOptions.token = token;
            handlerOptions.now = [this] { return now; };
            handlerOptions.log = [this] (const std::string& line) { logLines.push_back (line); };
            return handlerOptions;
        }

        Connection connect()
        {
            const auto id = nextConnectionId++;
            handler.connectionOpened (id);
            return { handler, id };
        }

        Connection connectAs (const std::string& session)
        {
            auto connection = connect();
            REQUIRE (connection.hello (session).contains ("result"));
            return connection;
        }

        bool logged (std::string_view part) const
        {
            return std::ranges::any_of (logLines, [part] (const std::string& line) { return contains (line, part); });
        }
    };

    int errorCode (const Json& response)
    {
        REQUIRE_MESSAGE (response.contains ("error"), response.dump());
        return response.at ("error").at ("code").get<int>();
    }

    std::string errorMessage (const Json& response)
    {
        REQUIRE_MESSAGE (response.contains ("error"), response.dump());
        return response.at ("error").at ("message").get<std::string>();
    }
}

//==============================================================================
TEST_CASE ("hello with the token lets a client in")
{
    Fixture fixture;
    auto client = fixture.connect();

    const auto response = client.hello ("session-1", "drums");
    CHECK (response.at ("id") == 1);
    CHECK (response.at ("result") == Json { { "server", "Hocket" }, { "version", api::hocketVersion() }, { "author", "Claude (drums)" } });
    CHECK_FALSE (client.closed);
    CHECK (fixture.logged ("Claude (drums) connected (session session-1)"));

    CHECK (client.request ("tools/list").at ("result").at ("tools").size() == 9);
}

TEST_CASE ("anything before hello, or a wrong token, is refused and the connection closed")
{
    Fixture fixture;

    SUBCASE ("another request first")
    {
        auto client = fixture.connect();
        const auto response = client.request ("tools/list");
        CHECK (errorCode (response) == -32001);
        CHECK (errorMessage (response) == "send hello with the token first; nothing else is accepted before it");
        CHECK (client.closed);
    }

    SUBCASE ("a wrong token")
    {
        auto client = fixture.connect();
        const auto response = client.request ("hello", { { "token", "guess" }, { "client", "Claude" }, { "session", "s" } });
        CHECK (errorCode (response) == -32001);
        CHECK (contains (errorMessage (response), "the token is wrong"));
        CHECK (client.closed);
        CHECK (fixture.logged ("refused a connection: the token is wrong"));
    }

    SUBCASE ("no token")
    {
        auto client = fixture.connect();
        CHECK (errorCode (client.request ("hello", { { "client", "Claude" }, { "session", "s" } })) == -32602);
        CHECK (client.closed);
    }

    SUBCASE ("text that isn't JSON")
    {
        auto client = fixture.connect();
        const auto reply = client.send ("GET / HTTP/1.1");
        CHECK (errorCode (Json::parse (reply.text)) == -32700);
        CHECK (reply.close);
    }

    SUBCASE ("a hello sent as a notification can't be answered")
    {
        auto client = fixture.connect();
        const auto reply = client.send (Json { { "jsonrpc", "2.0" }, { "method", "hello" },
                                               { "params", helloParams ("s") } }.dump());
        CHECK (reply.text.empty());
        CHECK (reply.close);
    }
}

TEST_CASE ("hello checks the client's name, session and label")
{
    Fixture fixture;
    const auto refused = [&fixture] (const Json& params)
    {
        auto client = fixture.connect();
        auto withBuild = params;
        withBuild["token"] = token;
        withBuild["version"] = api::hocketVersion();
        withBuild["fingerprint"] = api::buildFingerprint();
        const auto response = client.request ("hello", withBuild);
        CHECK (client.closed);
        return errorMessage (response);
    };

    CHECK (contains (refused ({ { "session", "s" } }), "hello needs \"client\""));
    CHECK (contains (refused ({ { "client", "" }, { "session", "s" } }), "hello needs \"client\""));
    CHECK (contains (refused ({ { "client", "Claude" } }), "hello needs \"session\""));
    CHECK (contains (refused ({ { "client", "Claude" }, { "session", "s\n" } }), "hello needs \"session\""));
    CHECK (contains (refused ({ { "client", "Claude" }, { "session", "s" }, { "label", 3 } }), "\"label\" is optional"));
    CHECK (contains (refused ({ { "client", "Claude" }, { "session", "s" }, { "colour", "red" } }), "hello has no parameter 'colour'"));
}

TEST_CASE ("hello turns away a client from another build")
{
    Fixture fixture;
    auto params = helloParams ("s");
    const auto version = std::string (api::hocketVersion());
    const auto fix = " Rebuild hocket-mcp from the same source as Hocket and reinstall it, then restart the MCP client so it starts the new hocket-mcp.";

    const auto refused = [&fixture] (const Json& hello)
    {
        auto client = fixture.connect();
        const auto response = client.request ("hello", hello);
        CHECK (client.closed);
        CHECK (errorCode (response) == -32002);
        return errorMessage (response);
    };

    SUBCASE ("the same version with other tools or another protocol")
    {
        params["fingerprint"] = "0123456789abcdef";
        CHECK (refused (params) == "hocket-mcp and this Hocket come from different builds of version " + version + ": their tools or protocol differ." + fix);
        CHECK (fixture.logged ("refused a connection: hocket-mcp and this Hocket come from different builds"));
    }

    SUBCASE ("another version")
    {
        params["version"] = "9.9.9";
        CHECK (refused (params) == "hocket-mcp 9.9.9 and this Hocket (" + version + ") come from different builds." + fix);
    }

    SUBCASE ("a client from before the check, which sends neither")
    {
        params.erase ("version");
        params.erase ("fingerprint");
        params["colour"] = "red"; // another build's hello may have other params too; the build is checked first
        CHECK (refused (params) == "hocket-mcp and this Hocket (" + version + ") come from different builds: hocket-mcp's hello has no version or fingerprint." + fix);
    }

    SUBCASE ("the token is checked first, so a stranger learns nothing about the build")
    {
        params["token"] = "guess";
        params["fingerprint"] = "0123456789abcdef";
        auto client = fixture.connect();
        CHECK (errorCode (client.request ("hello", params)) == -32001);
    }
}

TEST_CASE ("after hello, mistakes get errors but the connection stays open")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    CHECK (errorCode (client.request ("hello", { { "token", token } })) == -32600);
    CHECK (errorCode (Json::parse (client.send ("{").text)) == -32700);
    CHECK (errorCode (Json::parse (client.send ("[]").text)) == -32600);
    CHECK (errorCode (Json::parse (client.send (R"({"jsonrpc": "2.0", "id": 5})").text)) == -32600);
    CHECK (errorMessage (client.request ("resources/list")) == "there's no method 'resources/list'; the methods are hello, tools/list and tools/call");
    CHECK (client.send (R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").text.empty());

    const auto unknownTool = client.request ("tools/call", { { "name", "delete_everything" } });
    CHECK (errorCode (unknownTool) == -32602);
    CHECK (contains (errorMessage (unknownTool), "there's no tool 'delete_everything'; the tools are get_project_overview, read_clip, write_clip"));

    CHECK_FALSE (client.closed);
}

TEST_CASE ("tools/list gives every tool a description and an input schema")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");
    const auto tools = client.request ("tools/list").at ("result").at ("tools");

    std::set<std::string> names;

    for (const auto& tool : tools)
    {
        names.insert (tool.at ("name").get<std::string>());
        CHECK_FALSE (tool.at ("description").get<std::string>().empty());
        CHECK (tool.at ("inputSchema").at ("type") == "object");
    }

    CHECK (names == std::set<std::string> { "get_project_overview", "read_clip", "write_clip", "undo", "history",
                                            "begin_transaction", "commit", "rollback", "transport" });

    const auto begin = std::find_if (tools.begin(), tools.end(), [] (const Json& tool) { return tool.at ("name") == "begin_transaction"; });
    CHECK (contains (begin->at ("description").get<std::string>(), "send no requests for 10 minutes"));
}

//==============================================================================
TEST_CASE ("get_project_overview summarizes the project")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    CHECK (client.text ("get_project_overview")
           == "Tempo 120 BPM, 4/4. The song is 2 bars long, from 1.1 to 3.1. There's no audio engine here, so it can't play.\n"
              "\n"
              "Tracks, in display order:\n"
              "- trk-bass \"Bass\" (midi), devices: dev-bass-synth (tracktion:4osc)\n"
              "  - clip clp-bass: 1.1 to 3.1 (@length 2bars), 14 notes, file clips/bass.notes\n"
              "\n"
              "History: empty; nothing has changed since Hocket opened the project.\n"
              "No transaction is open.\n");

    client.call ("begin_transaction", { { "name", "wrote bridge" } });
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });
    client.call ("commit");

    const auto overview = client.text ("get_project_overview");
    CHECK (contains (overview, "History: 1 entry; the latest is #1 \"wrote bridge\" by you.\n"));
    CHECK (contains (overview, "1.1 to 2.1 (@length 1bar), 1 note"));
}

TEST_CASE ("get_project_overview says whether the song is playing")
{
    struct FakeTransport final : commands::Transport
    {
        commands::Result play() override { playing = true; return { true, "playing" }; }
        commands::Result stop() override { playing = false; return { true, "stopped" }; }
        bool isPlaying() const override { return playing; }

        bool playing = false;
    };

    Fixture fixture;
    FakeTransport transport;
    fixture.bus.setTransport (&transport);
    auto client = fixture.connectAs ("session-1");

    CHECK (contains (client.text ("get_project_overview"), "from 1.1 to 3.1. It's stopped; transport play loops it.\n"));
    client.call ("transport", { { "action", "play" } });
    CHECK (contains (client.text ("get_project_overview"), "from 1.1 to 3.1. It's playing, on a loop.\n"));

    fixture.bus.setTransport (nullptr);
}

TEST_CASE ("get_project_overview lists a track's clips in song order")
{
    auto project = demoProject();
    auto& clips = project.tracks[0].clips;
    auto later = clips[0];
    later.id = "clp-later";
    later.file = "clips/later.notes";
    later.start = 4 * 3840;
    clips.insert (clips.begin(), later); // listed first, but starts at bar 5

    commands::CommandBus bus (std::move (project));
    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };
    REQUIRE (connection.hello().contains ("result"));

    const auto overview = connection.text ("get_project_overview");
    const auto first = overview.find ("clip clp-bass: 1.1");
    const auto second = overview.find ("clip clp-later: 5.1");
    REQUIRE (first != std::string::npos);
    REQUIRE (second != std::string::npos);
    CHECK (first < second);
}

TEST_CASE ("read_clip gives the stored text verbatim, in a block of its own")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    const auto result = client.call ("read_clip", { { "clip_id", "clp-bass" } });
    CHECK (result.at ("isError") == false);
    REQUIRE (result.at ("content").size() == 2);
    CHECK (result.at ("content")[0].at ("text")
           == "clip clp-bass on track trk-bass \"Bass\"\n"
              "file: clips/bass.notes\n"
              "from 1.1 to 3.1 (@length 2bars), 14 notes\n"
              "The next block is the clip's .notes text, exactly as stored. To change the clip, edit that text and send all of it to write_clip.");
    CHECK (result.at ("content")[1].at ("text") == demoClipText()); // the example is in canonical form
    CHECK (fixture.logLines.size() == 1); // just the connection: reading isn't logged, only failing to

    const auto unknown = client.call ("read_clip", { { "clip_id", "trk-bass" } });
    CHECK (unknown.at ("isError") == true);
    CHECK (unknown.at ("content")[0].at ("text") == "read_clip: 'trk-bass' is a track ID; read_clip needs a clip ID like 'clp-bass'");
    CHECK (fixture.logged ("Claude: read_clip failed: read_clip: 'trk-bass' is a track ID"));
}

TEST_CASE ("write_clip names the new entry and says whether the text was stored as sent")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    SUBCASE ("canonical text")
    {
        CHECK (client.text ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1      C2    1/4   v100\n" } })
               == "#1 wrote clip clp-bass (1 note)\nStored exactly as you sent it.");
        CHECK (fixture.logged ("Claude: write_clip: #1 wrote clip clp-bass (1 note)"));
    }

    SUBCASE ("text that's stored differently")
    {
        CHECK (client.text ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.3 E2 1/4 v100\n1.2 D2 1/4 v100\n1.1 C2 2/8 v100\n" } })
               == "#1 wrote clip clp-bass (3 notes)\n"
                  "Stored in canonical form, which differs from what you sent: rewrote one line: `1.1 C2 2/8 v100` became "
                  "`1.1      C2    1/4   v100`; sorted the lines by position; changed spacing (columns are fixed-width). "
                  "read_clip returns the stored text.");
    }

    SUBCASE ("the same notes again")
    {
        CHECK (client.text ("write_clip", { { "clip_id", "clp-bass" }, { "notes", demoClipText() } })
               == "clip 'clp-bass' already has exactly these notes, so nothing changed and no history entry was added\n"
                  "Stored exactly as you sent it.");
    }

    SUBCASE ("invalid text changes nothing")
    {
        const auto result = client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.5 C2 1/4 v100\n" } });
        CHECK (result.at ("isError") == true);
        CHECK (contains (result.at ("content")[0].at ("text").get<std::string>(), "\nclips/bass.notes:3:1: '1.5' isn't a valid position"));
        CHECK (fixture.bus.history().empty());
        CHECK (fixture.logged ("Claude: write_clip failed: write_clip: the new text for clip 'clp-bass' has 1 error"));
    }
}

TEST_CASE ("tool arguments are checked, and the errors say how to fix them")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    const auto error = [&client] (const std::string& tool, const Json& arguments)
    {
        const auto result = client.call (tool, arguments);
        CHECK (result.at ("isError") == true);
        return result.at ("content")[0].at ("text").get<std::string>();
    };

    CHECK (error ("write_clip", { { "clip_id", "clp-bass" } }) == "write_clip: missing argument 'notes' (a string)");
    CHECK (error ("write_clip", { { "clip", "clp-bass" } }) == "write_clip: there's no argument 'clip'; its arguments are 'clip_id', 'notes'");
    CHECK (error ("undo", { { "force", "yes" } }) == "undo: 'force' must be true or false, not \"yes\"");
    CHECK (error ("history", { { "limit", 0 } }) == "history: 'limit' must be from 1 to 100, not 0");
    CHECK (error ("commit", { { "name", "x" } }) == "commit: there's no argument 'name'; it takes no arguments");
    CHECK (error ("transport", { { "action", "rewind" } }) == "transport: 'action' must be \"play\" or \"stop\", not \"rewind\"");

    // Leaving out "arguments" altogether is the same as {}.
    CHECK (client.request ("tools/call", { { "name", "history" } }).at ("result").at ("isError") == false);
}

TEST_CASE ("undo and history tell clients apart, even with the same name")
{
    Fixture fixture;
    auto mine = fixture.connectAs ("session-1");
    auto other = fixture.connectAs ("session-2");

    other.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });

    const auto refused = mine.call ("undo");
    CHECK (refused.at ("isError") == true);
    CHECK (contains (refused.at ("content")[0].at ("text").get<std::string>(),
                     "#1 \"wrote clip clp-bass (1 note)\", was made by Claude in another session, not this one."));

    CHECK (mine.text ("history") == "The only entry:\n#1 Claude in another session: wrote clip clp-bass (1 note)");
    CHECK (other.text ("history") == "The only entry:\n#1 you: wrote clip clp-bass (1 note)");

    CHECK (mine.text ("undo", { { "force", true } }) == "undid #1 \"wrote clip clp-bass (1 note)\" by Claude");
    CHECK (fixture.bus.project() == demoProject());
}

TEST_CASE ("transactions over the API")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    CHECK (client.text ("begin_transaction", { { "name", "wrote bridge" } }) == "began transaction \"wrote bridge\"");
    CHECK (contains (client.text ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } }),
                     "wrote clip clp-bass (1 note) in your open transaction \"wrote bridge\"; it becomes a history entry when you commit"));
    CHECK (client.text ("history") == "The history is empty: nothing has changed since Hocket opened the project.\n"
                                      "Not listed yet: the edits in the open transaction \"wrote bridge\" by you, until it's committed.");
    CHECK (client.text ("commit") == "#1 wrote bridge");

    client.call ("begin_transaction", { { "name", "tried something" } });
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 D2 1/4 v100\n" } });
    CHECK (client.text ("rollback") == "rolled back \"tried something\"");
    CHECK (fixture.bus.history().size() == 1);
}

TEST_CASE ("transport fails clearly without an engine")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    const auto result = client.call ("transport", { { "action", "play" } });
    CHECK (result.at ("isError") == true);
    CHECK (result.at ("content")[0].at ("text") == "play: there's no audio engine running, so playback can't be started or stopped here");
}

//==============================================================================
TEST_CASE ("a disconnect commits the open transaction as interrupted, and the client hears about it when it's back")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    client.call ("begin_transaction", { { "name", "wrote bridge" } });
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });
    client.close();

    CHECK (fixture.bus.history() == std::vector<commands::HistoryEntry> { { 1, { "Claude", "session-1" }, "wrote bridge (interrupted)" } });
    CHECK_FALSE (fixture.bus.openTransaction().has_value());
    CHECK (fixture.logged ("Claude: transaction \"wrote bridge\" was closed; its edits are #1 \"wrote bridge (interrupted)\" (disconnected)"));

    // The same session reconnects: it's still the author, so it can undo the entry.
    auto back = fixture.connectAs ("session-1");
    const auto undone = back.call ("undo");
    CHECK (undone.at ("isError") == false);
    REQUIRE (undone.at ("content").size() == 2);
    CHECK (undone.at ("content")[0].at ("text") == "undid #1 \"wrote bridge (interrupted)\" by Claude");
    CHECK (undone.at ("content")[1].at ("text")
           == "Note: when you disconnected, your transaction \"wrote bridge\" was closed; its edits are #1 \"wrote bridge (interrupted)\"");

    // The note is told once.
    CHECK (back.call ("history").at ("content").size() == 1);
}

TEST_CASE ("an empty transaction closes without an entry when its client disconnects")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");
    client.call ("begin_transaction", { { "name", "wrote bridge" } });
    client.close();

    CHECK (fixture.bus.history().empty());
    CHECK_FALSE (fixture.bus.openTransaction().has_value());
}

TEST_CASE ("a disconnect leaves the transaction open while the client has another connection")
{
    Fixture fixture;
    auto first = fixture.connectAs ("session-1");
    auto second = fixture.connectAs ("session-1");

    first.call ("begin_transaction", { { "name", "wrote bridge" } });
    first.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });
    first.close();

    CHECK (fixture.bus.openTransaction().has_value());
    CHECK (second.text ("commit") == "#1 wrote bridge");
}

TEST_CASE ("another client's disconnect doesn't touch the transaction")
{
    Fixture fixture;
    auto owner = fixture.connectAs ("session-1");
    auto other = fixture.connectAs ("session-2");

    owner.call ("begin_transaction", { { "name", "wrote bridge" } });
    other.close();
    CHECK (fixture.bus.openTransaction().has_value());

    // A connection that never said hello isn't anyone.
    auto stranger = fixture.connect();
    stranger.close();
    CHECK (fixture.bus.openTransaction().has_value());
}

TEST_CASE ("the idle timeout commits a transaction only after the owner has been quiet that long")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");
    auto bystander = fixture.connectAs ("session-2");

    client.call ("begin_transaction", { { "name", "wrote bridge" } });
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });

    fixture.now += 9min;
    fixture.handler.checkIdle();
    CHECK (fixture.bus.openTransaction().has_value());

    // Any request from the owner restarts the clock; other clients' don't.
    client.call ("read_clip", { { "clip_id", "clp-bass" } });
    fixture.now += 9min;
    bystander.call ("history");
    fixture.handler.checkIdle();
    CHECK (fixture.bus.openTransaction().has_value());

    fixture.now += 1min;
    fixture.handler.checkIdle();
    CHECK_FALSE (fixture.bus.openTransaction().has_value());
    CHECK (fixture.bus.history().front().description == "wrote bridge (interrupted)");
    CHECK (fixture.logged ("(no request for 10 minutes)"));

    // A late commit says what happened.
    const auto late = client.call ("commit");
    CHECK (late.at ("isError") == true);
    CHECK (late.at ("content")[0].at ("text") == "you don't have an open transaction to commit");
    CHECK (late.at ("content")[1].at ("text")
           == "Note: after 10 minutes with no request from you, your transaction \"wrote bridge\" was closed; its edits are #1 \"wrote bridge (interrupted)\"");
}

TEST_CASE ("the idle timeout leaves the person at the DAW alone")
{
    Fixture fixture;
    fixture.bus.beginTransaction (commands::Author { "Lee" }, "by hand");

    fixture.now += 60min;
    fixture.handler.checkIdle();
    CHECK (fixture.bus.openTransaction().has_value());
}

TEST_CASE ("an internal error is answered as one, and the connection carries on")
{
    // Anything that throws while handling a request, here the clock.
    commands::CommandBus bus { demoProject() };
    bool failing = false;
    std::vector<std::string> logLines;

    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    handlerOptions.now = [&failing]
    {
        if (failing)
            throw std::runtime_error ("the clock broke");

        return api::Clock::time_point {};
    };
    handlerOptions.log = [&logLines] (const std::string& line) { logLines.push_back (line); };

    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };
    REQUIRE (connection.hello().contains ("result"));

    failing = true;
    const auto response = connection.request ("tools/call", { { "name", "history" } });
    CHECK (response.at ("id") == 2);
    CHECK (response.at ("error").at ("code") == -32603);
    CHECK (response.at ("error").at ("message")
           == "Hocket hit an internal error while handling this request (the clock broke), so it may or may not have been "
              "carried out; check history before trying again");
    CHECK_FALSE (connection.closed);
    CHECK (contains (logLines.back(), "internal error: the clock broke"));

    failing = false;
    CHECK (connection.call ("history").at ("isError") == false);
}

TEST_CASE ("an internal error before hello closes the connection")
{
    commands::CommandBus bus { demoProject() };
    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    handlerOptions.now = []() -> api::Clock::time_point { throw std::runtime_error ("the clock broke"); };

    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };

    const auto response = connection.hello();
    CHECK (response.at ("error").at ("code") == -32603);
    CHECK (connection.closed);
}
