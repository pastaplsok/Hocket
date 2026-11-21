#include "api/Handler.h"
#include "api/Protocol.h"
#include "core/Paths.h"
#include "core/project/Project.h"
#include "common/FakeStorage.h"
#include "common/TestKits.h"

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

    CHECK (client.request ("tools/list").at ("result").at ("tools").size() == 22);
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

    CHECK (names == std::set<std::string> { "get_project_overview", "read_clip", "write_clip", "patch_clip", "add_clip", "move_clip", "remove_clip",
                                            "make_clip_unique", "add_track", "set_track", "remove_track", "set_song", "transport", "undo",
                                            "redo", "history", "begin_transaction", "commit", "rollback", "save", "list_saves", "revert" });

    // Only the reading tools are marked read-only (SPEC 6.4).
    std::set<std::string> readOnly;

    for (const auto& tool : tools)
        if (tool.at ("annotations").at ("readOnlyHint").get<bool>())
            readOnly.insert (tool.at ("name").get<std::string>());

    CHECK (readOnly == std::set<std::string> { "get_project_overview", "read_clip", "history", "list_saves" });

    // The standard drum map is in write_clip's description.
    const auto write = std::find_if (tools.begin(), tools.end(), [] (const Json& tool) { return tool.at ("name") == "write_clip"; });
    CHECK (contains (write->at ("description").get<std::string>(), "kick2=35, kick=36, sidestick=37, snare=38,"));
    CHECK (contains (write->at ("description").get<std::string>(), "triangle-mute=80, triangle-open=81."));

    const auto begin = std::find_if (tools.begin(), tools.end(), [] (const Json& tool) { return tool.at ("name") == "begin_transaction"; });
    CHECK (contains (begin->at ("description").get<std::string>(), "send no requests for 10 minutes"));
}

//==============================================================================
TEST_CASE ("get_project_overview summarizes the project")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    CHECK (client.text ("get_project_overview")
           == "Tempo 120 BPM, 4/4. The song is 2 bars long, from 1.1 to 3.1.\n"
              "Transport: there's no audio engine here, so it can't play.\n"
              "\n"
              "Tracks, in display order:\n"
              "- trk-bass \"Bass\" (midi; volume 0dB, pan C), devices: dev-bass-synth (hocket:synth)\n"
              "  - clip clp-bass: 1.1 to 3.1 (@length 2bars), 14 notes\n"
              "\n"
              "Installed kits: none, so a drum sampler has nothing to play.\n"
              "\n"
              "History: empty; nothing has changed since Hocket opened the project.\n"
              "No transaction is open. Nothing is unsaved.\n");

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
        void play() override { playing = true; }
        void stop() override { playing = false; }
        bool isPlaying() const override { return playing; }
        notes::Ticks playheadPosition() const override { return 0; }
        void jumpTo (notes::Ticks) override {}
        void loopChanged() override {}

        bool playing = false;
    };

    Fixture fixture;
    FakeTransport transport;
    fixture.bus.setTransport (&transport);
    auto client = fixture.connectAs ("session-1");

    CHECK (contains (client.text ("get_project_overview"),
                     "from 1.1 to 3.1.\nTransport: stopped. Play plays the whole song, 1.1 to 3.1, on a loop, from 1.1.\n"
                     "Output: nothing has played since Hocket started.\n"));
    client.call ("transport", { { "action", "play" } });
    CHECK (contains (client.text ("get_project_overview"), "Transport: playing the whole song, 1.1 to 3.1, on a loop; the playhead is at 1.1.\n"));

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
    project.notesFiles["clips/later.notes"] = project.notesFiles.at ("clips/bass.notes");

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

TEST_CASE ("the overview and read_clip show linked clips, mix settings, and what a beat is in 6/8")
{
    auto project = demoProject();
    project.meter = { 6, 8 };
    project.notesFiles["clips/bass.notes"] = *notes::parse ("@length 1bar\n\n1.1 C2 1/8 v100\n", project.meter).clip;
    project.tracks[0].clips.push_back ({ "clp-bass-again", "clips/bass.notes", 2 * 2880 });

    auto& lead = project.tracks.emplace_back();
    lead = { "trk-lead", "Lead", project::TrackType::midi, {}, { { "clp-lead", "clips/bass.notes", 2880 } } };
    lead.volume = -6.5;
    lead.pan = -20;
    lead.mute = true;
    lead.solo = true;

    commands::CommandBus bus (std::move (project));
    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };
    REQUIRE (connection.hello().contains ("result"));

    CHECK (connection.text ("get_project_overview")
           == "Tempo 120 BPM, 6/8 (a beat is an eighth note; BPM counts quarter notes). The song is 3 bars long, from 1.1 to 4.1.\n"
              "Transport: there's no audio engine here, so it can't play.\n"
              "\n"
              "Tracks, in display order:\n"
              "- trk-bass \"Bass\" (midi; volume 0dB, pan C), devices: dev-bass-synth (hocket:synth)\n"
              "  - clip clp-bass: 1.1 to 2.1 (@length 1bar), 1 note, file clips/bass.notes (linked with clp-bass-again, clp-lead)\n"
              "  - clip clp-bass-again: 3.1 to 4.1 (@length 1bar), 1 note, file clips/bass.notes (linked with clp-bass, clp-lead)\n"
              "- trk-lead \"Lead\" (midi; volume -6.5dB, pan L20, muted, soloed), devices: none\n"
              "  - clip clp-lead: 2.1 to 3.1 (@length 1bar), 1 note, file clips/bass.notes (linked with clp-bass, clp-bass-again)\n"
              "\n"
              "Installed kits: none, so a drum sampler has nothing to play.\n"
              "\n"
              "History: empty; nothing has changed since Hocket opened the project.\n"
              "No transaction is open. Nothing is unsaved.\n");

    const auto read = connection.call ("read_clip", { { "clip_id", "clp-lead" } });
    CHECK (read.at ("content")[0].at ("text")
           == "clip clp-lead on track trk-lead \"Lead\"\n"
              "file: clips/bass.notes (linked with clp-bass, clp-bass-again)\n"
              "from 2.1 to 3.1 (@length 1bar), 1 note\n"
              "The next block is the clip's .notes text, exactly as stored. To change the clip, edit that text and send all of it to write_clip, "
              "or send the notes that change to patch_clip; its linked clips change with it, since they share the file.");

    const auto written = connection.call ("write_clip", { { "clip_id", "clp-lead" }, { "notes", "@length 1bar\n\n1.6.2 C2 1/16 v100\n" } });
    CHECK (written.at ("isError") == false);
    CHECK (contains (written.at ("content")[0].at ("text").get<std::string>(),
                     "#1 wrote clip clp-lead (1 note), and its linked clips clp-bass, clp-bass-again"));
}

TEST_CASE ("on a drums track: the overview shows the kit and drum map, and clips use its names")
{
    project::Track drums { "trk-drums", "Drums", project::TrackType::drums, { { "dev-drums-kit", project::DeviceType::drumSampler, "acoustic" } },
                           { { "clp-beat", "clips/beat.notes", 0 } } };
    drums.drumMap.add ("kick", 36);
    drums.drumMap.add ("snare", 38);
    drums.drumMap.add ("hat-closed", 42);

    project::Project project;
    project.notesFiles["clips/beat.notes"] = *notes::parse ("@length 1bar\n\n1.1 [C2 F#2] 1/16 v100\n1.2 D2 1/16 v100\n").clip;
    project.tracks.push_back (std::move (drums));

    commands::CommandBus bus (std::move (project));
    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };
    REQUIRE (connection.hello().contains ("result"));

    // A map far from the standard one is listed whole; the kit's line says why the track is silent.
    const auto overview = connection.text ("get_project_overview");
    CHECK (contains (overview, "- trk-drums \"Drums\" (drums; volume 0dB, pan C), devices: dev-drums-kit (hocket:drum-sampler, kit \"acoustic\"), "
                               "drum map: empty, kick=36, snare=38, hat-closed=42\n"
                               "  - clip clp-beat: 1.1 to 2.1 (@length 1bar), 3 notes\n"
                               "\n"
                               "Kits in use, with the drum map names each plays on its track (the map's other names are silent):\n"
                               "- \"acoustic\" on trk-drums: kit 'acoustic' isn't installed. No kits are installed: bundled kits go in kits\\ next to "
                               "Hocket's programs, and the user's own in Documents\\Hocket\\Kits, named \"user/<folder>\", so the track is silent\n"
                               "Installed kits: none, so a drum sampler has nothing to play.\n"));

    const auto read = connection.call ("read_clip", { { "clip_id", "clp-beat" } });
    CHECK (contains (read.at ("content")[0].at ("text").get<std::string>(),
                     "Its notes are named by trk-drums's drum map, which get_project_overview shows; notes the map doesn't name are written as note names.\n"));
    CHECK (read.at ("content")[1].at ("text") == "@length 1bar\n\n1.1      [kick hat-closed]  1/16  v100\n1.2      snare 1/16  v100\n");

    // Notes the map doesn't name are listed, and the kit isn't installed, so none sound.
    const auto written = connection.call ("write_clip", { { "clip_id", "clp-beat" },
                                                          { "notes", "@length 1bar\n\n1.1      kick  1/16  v100\n1.2      Bb5   1/16  v90\n"
                                                                     "1.3      Bb5   1/16  v90\n1.4      C7    1/16  v90\n" } });
    CHECK (written.at ("isError") == false);
    CHECK (written.at ("content")[0].at ("text")
           == "#1 wrote clip clp-beat (4 notes)\n"
              "Stored exactly as you sent it.\n"
              "These notes have no name in trk-drums's drum map, so they're written as note names: Bb5 (note 82, 2 notes), C7 (note 96).\n"
              "None of these notes sound: kit 'acoustic' isn't installed. No kits are installed: bundled kits go in kits\\ next to Hocket's "
              "programs, and the user's own in Documents\\Hocket\\Kits, named \"user/<folder>\", so trk-drums is silent.");

    // A note name the map names is stored as the name.
    const auto renamed = connection.call ("write_clip", { { "clip_id", "clp-beat" }, { "notes", "@length 1bar\n\n1.1      D2    1/16  v100\n" } });
    CHECK (contains (renamed.at ("content")[0].at ("text").get<std::string>(), "Stored in canonical form, which differs from what you sent"));
    CHECK_FALSE (contains (renamed.at ("content")[0].at ("text").get<std::string>(), "no name in"));
    CHECK (connection.call ("read_clip", { { "clip_id", "clp-beat" } }).at ("content")[1].at ("text") == "@length 1bar\n\n1.1      snare 1/16  v100\n");
}

TEST_CASE ("write_clip on a drums track names the clip's notes that its kit has no piece for")
{
    const auto piece = [] (std::string name, int note) { return kits::Piece { std::move (name), note, "x.wav", 0.0 }; };

    kits::KitCatalog catalog;
    catalog.add ({ "electronic", kits::Source::bundled, {}, { "Electronic, 808-style", { piece ("kick", 36), piece ("snare", 38), piece ("perc", 82) }, {} } });

    project::Track drums { "trk-drums", "Drums", project::TrackType::drums, { { "dev-drums-kit", project::DeviceType::drumSampler, "electronic" } },
                           { { "clp-beat", "clips/beat.notes", 0 } } };
    drums.drumMap = notes::standardDrumMap();
    project::Track keys { "trk-keys", "Keys", project::TrackType::drums, { { "dev-keys-synth", project::DeviceType::synth } },
                          { { "clp-keys", "clips/keys.notes", 0 } } };
    keys.drumMap = notes::standardDrumMap();

    project::Project project;
    project.notesFiles["clips/beat.notes"] = *notes::parse ("@length 1bar\n").clip;
    project.notesFiles["clips/keys.notes"] = *notes::parse ("@length 1bar\n").clip;
    project.tracks.push_back (std::move (drums));
    project.tracks.push_back (std::move (keys));

    commands::CommandBus bus (std::move (project));
    bus.setKitCatalog (&catalog);
    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };
    REQUIRE (connection.hello().contains ("result"));

    const auto beat = "@length 1bar\n\n1.1      [kick hat-closed]  1/16  v100\n1.2      snare 1/16  v90\n1.3      hat-closed  1/16  v80\n"
                      "1.4      [A#5 C7]  1/16  v90\n";
    CHECK (connection.call ("write_clip", { { "clip_id", "clp-beat" }, { "notes", beat } }).at ("content")[0].at ("text")
           == "#1 wrote clip clp-beat (6 notes)\n"
              "Stored exactly as you sent it.\n"
              "These notes have no name in trk-drums's drum map, so they're written as note names: A#5 (note 82), C7 (note 96).\n"
              "Kit \"electronic\" has no piece for these, so they're silent: hat-closed (2 notes), C7 (note 96).");

    // Everything plays: nothing more to say.
    CHECK (connection.call ("write_clip", { { "clip_id", "clp-beat" }, { "notes", "@length 1bar\n\n1.1      kick  1/16  v100\n" } })
               .at ("content")[0].at ("text")
           == "#2 wrote clip clp-beat (1 note)\nStored exactly as you sent it.");

    // A drums track played by a synth has no kit to miss pieces.
    CHECK (connection.call ("write_clip", { { "clip_id", "clp-keys" }, { "notes", "@length 1bar\n\n1.1      hat-closed  1/16  v100\n" } })
               .at ("content")[0].at ("text")
           == "#3 wrote clip clp-keys (1 note)\nStored exactly as you sent it.");
}

TEST_CASE ("the overview says where each track's kit came from, and which of the drum map's names it plays")
{
    const auto piece = [] (std::string name, int note) { return kits::Piece { std::move (name), note, "x.wav", 0.0 }; };

    kits::KitCatalog catalog;
    catalog.add ({ "electronic", kits::Source::bundled, {}, { "Electronic, 808-style", { piece ("kick", 36), piece ("snare-tight", 38), piece ("hat-open", 46) }, {} } });
    catalog.add ({ "user/dusty-909", kits::Source::user, pathFromUtf8 ("C:\\Users\\Me\\Documents\\Hocket\\Kits\\dusty-909"),
                   { "My 909", { piece ("kick", 36), piece ("snare", 38), piece ("hat-closed", 42) }, {} } });

    const auto drumTrack = [] (std::string id, std::string kit)
    {
        project::Track track { id, "Drums", project::TrackType::drums, { { "dev-" + id.substr (4) + "-kit", project::DeviceType::drumSampler, std::move (kit) } }, {} };
        track.drumMap.add ("kick", 36);
        track.drumMap.add ("snare", 38);
        track.drumMap.add ("hat-closed", 42);
        return track;
    };

    // A kit whose files changed into something unusable, with what its last load found.
    kits::Kit loud { "user/loud", kits::Source::user, pathFromUtf8 ("C:\\Kits\\loud"), { "Loud", { piece ("kick", 36), piece ("snare", 38) }, {} } };
    loud.problem = "kit.json has an error: kit.json:7:22: piece 'snare': \"note\" should be a MIDI note number from 0 to 127, e.g. 36";
    loud.loaded = { { "the sample for piece 'clap' couldn't be decoded" }, { { "kick", 2.31 }, { "snare", 0.04 } } };
    catalog.add (std::move (loud));

    project::Project project;
    project.tracks.push_back (drumTrack ("trk-e", "user/loud"));
    project.tracks.push_back (drumTrack ("trk-a", "electronic"));
    project.tracks.push_back (drumTrack ("trk-b", "user/dusty-909"));
    project.tracks.push_back (drumTrack ("trk-c", "dusty-909"));
    project.tracks.push_back (drumTrack ("trk-d", "electronic"));
    project.tracks.back().drumMap = {};

    commands::CommandBus bus (std::move (project));
    bus.setKitCatalog (&catalog);
    api::HandlerOptions handlerOptions;
    handlerOptions.token = token;
    api::Handler handler (bus, std::move (handlerOptions));
    handler.connectionOpened (1);
    Connection connection { handler, 1 };
    REQUIRE (connection.hello().contains ("result"));

    const auto overview = connection.text ("get_project_overview");
    CHECK (contains (overview, "\n- electronic on trk-a (bundled; Electronic, 808-style): kick, snare (the kit's 'snare-tight'). It also has pieces on "
                               "notes the drum map doesn't name, written as note names: A#2 (note 46, 'hat-open')\n"));
    CHECK (contains (overview, "\n- user/dusty-909 on trk-b (the user's own, in C:\\Users\\Me\\Documents\\Hocket\\Kits\\dusty-909; My 909): kick, snare, "
                               "hat-closed\n"));
    CHECK (contains (overview, "\n- \"dusty-909\" on trk-c: kit 'dusty-909' isn't installed (did you mean 'user/dusty-909'?). The installed kits are "
                               "'electronic' (bundled), 'user/dusty-909' (the user's own), 'user/loud' (the user's own), so the track is silent\n"));
    CHECK (contains (overview, "\n- user/loud on trk-e (the user's own, in C:\\Kits\\loud; Loud): kick, snare. Its files have changed since it loaded, "
                               "but can't be used as they are, so the version that loaded plays on: kit.json has an error: kit.json:7:22: piece 'snare': "
                               "\"note\" should be a MIDI note number from 0 to 127, e.g. 36. Loading it found: the sample for piece 'clap' couldn't be "
                               "decoded. Pieces' samples peak above full scale: kick at +2.31 dBFS, snare at +0.04 dBFS\n"));
    CHECK (contains (overview, "\n- electronic on trk-d (bundled; Electronic, 808-style): the drum map is empty, so it names none of the kit's pieces. "
                               "It also has pieces on notes the drum map doesn't name, written as note names: C2 (note 36, 'kick'), D2 (note 38, "
                               "'snare-tight'), A#2 (note 46, 'hat-open')\n"));
    CHECK (contains (overview, "drum map: empty\n"));
    CHECK (contains (overview, "\nInstalled kits: electronic (bundled, 3 pieces), user/dusty-909 (the user's, 3 pieces), user/loud (the user's, 2 pieces).\n"));
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
              "The next block is the clip's .notes text, exactly as stored. To change the clip, edit that text and send all of it to write_clip, "
              "or send the notes that change to patch_clip.");
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

TEST_CASE ("a notification from the owner restarts the idle clock too")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    client.call ("begin_transaction", { { "name", "wrote bridge" } });
    client.call ("write_clip", { { "clip_id", "clp-bass" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } });

    fixture.now += 9min;
    const auto reply = client.send (R"({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 7}})");
    CHECK (reply.text.empty());
    CHECK_FALSE (reply.close);

    fixture.now += 9min;
    fixture.handler.checkIdle();
    CHECK (fixture.bus.openTransaction().has_value());

    fixture.now += 1min;
    fixture.handler.checkIdle();
    CHECK_FALSE (fixture.bus.openTransaction().has_value());
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

//==============================================================================
// Step 10's tools (SPEC §6.4).

namespace
{
    std::string textOf (const Json& result)
    {
        return result.at ("content")[0].at ("text").get<std::string>();
    }

    bool failed (const Json& result)
    {
        return result.at ("isError").get<bool>();
    }

    // A transport whose playhead, progress and wraps a test sets.
    struct ToolTransport final : commands::Transport
    {
        void play() override { playing = true; }
        void stop() override { playing = false; }
        bool isPlaying() const override { return playing; }
        notes::Ticks playheadPosition() const override { return playhead; }
        void jumpTo (notes::Ticks position) override { playhead = position; }
        void loopChanged() override {}
        commands::PlayProgress playProgress() const override { return progress; }

        bool playing = false;
        notes::Ticks playhead = 0;
        commands::PlayProgress progress;
    };

    struct ToolMeter final : commands::OutputMeter
    {
        commands::OutputLevel levelSincePlay() const override { return level; }
        void restart() override { level = {}; }

        commands::OutputLevel level;
    };
}

TEST_CASE ("tracks: added, changed in one entry, with the drum map as changes to the standard one, and removed")
{
    Fixture fixture;
    kits::KitCatalog catalog;
    catalog.add (testing::testKit ("electronic", { { "kick", 36 }, { "snare", 38 }, { "hat-closed", 42 } }, "Electronic"));
    fixture.bus.setKitCatalog (&catalog);
    auto client = fixture.connectAs ("session-1");

    const auto added = client.call ("add_track", { { "type", "drums" }, { "name", "Drums" }, { "instrument", "hocket:drum-sampler" },
                                                   { "kit", "electronic" }, { "before", "trk-bass" } });
    CHECK_FALSE (failed (added));
    CHECK (textOf (added).starts_with ("#1 added track trk-drums \"Drums\" before trk-bass. Its instrument is dev-drums-kit, a hocket:drum-sampler "
                                       "playing kit 'electronic'."));

    auto overview = client.text ("get_project_overview");
    CHECK (contains (overview, "- trk-drums \"Drums\" (drums; volume 0dB, pan C), devices: dev-drums-kit (hocket:drum-sampler, kit \"electronic\"), "
                               "drum map: standard\n  - no clips\n"));
    CHECK (contains (overview, "\n- electronic on trk-drums (bundled; Electronic): kick, snare, hat-closed\n"));
    CHECK (contains (overview, "\nInstalled kits: electronic (bundled, 3 pieces).\n"));

    // Several settings, one entry; the overview's form of the map goes back in as it is.
    const auto changed = client.call ("set_track", { { "track_id", "trk-drums" }, { "name", "Kit" }, { "volume", "-6dB" },
                                                     { "drum_map", "rim=37, -cabasa" } });
    CHECK_FALSE (failed (changed));
    CHECK (textOf (changed).starts_with ("#2 renamed track trk-drums \"Drums\" to \"Kit\"; set trk-drums's volume to -6dB; changed trk-drums's drum map: "));
    CHECK (fixture.bus.history().size() == 2);
    CHECK (contains (client.text ("get_project_overview"), "drum map: standard, rim=37, -cabasa\n"));

    CHECK_FALSE (failed (client.call ("set_track", { { "track_id", "trk-drums" }, { "drum_map", "standard" } })));
    CHECK (contains (client.text ("get_project_overview"), "drum map: standard\n"));

    // Mistakes say how to write it.
    CHECK (textOf (client.call ("set_track", { { "track_id", "trk-drums" } })).starts_with ("set_track: give at least one of"));
    CHECK (contains (textOf (client.call ("set_track", { { "track_id", "trk-drums" }, { "drum_map", "rim:37" } })), "\"rim:37\" isn't a change"));
    CHECK (contains (textOf (client.call ("set_track", { { "track_id", "trk-drums" }, { "drum_map", "rim=x" } })), "needs a MIDI note number"));
    CHECK (contains (textOf (client.call ("set_track", { { "track_id", "trk-drums" }, { "drum_map", "rim=37, standard" } })), "must come first"));
    CHECK (textOf (client.call ("add_track", { { "type", "drums" }, { "name", "More" }, { "kit", "electronic" } }))
           == "add_track: a kit goes with the instrument \"hocket:drum-sampler\"; give both");

    CHECK (textOf (client.call ("set_track", { { "track_id", "trk-bass" }, { "instrument", "none" } })) == "#4 removed trk-bass's instrument dev-bass-synth");
    CHECK (textOf (client.call ("remove_track", { { "track_id", "trk-drums" } })) == "#5 removed track trk-drums \"Kit\"");

    fixture.bus.setKitCatalog (nullptr);
}

TEST_CASE ("clips: added, linked, made unique, moved and removed, and a file shown only when the ID doesn't imply it")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    CHECK (textOf (client.call ("add_clip", { { "track_id", "trk-bass" }, { "start", "3.1" }, { "bars", 2 } }))
           == "#1 added clip clp-bass-2 on trk-bass at 3.1 (empty, 2bars). Its notes are in clips/bass-2.notes");

    const auto linked = textOf (client.call ("add_clip", { { "track_id", "trk-bass" }, { "start", "5.1" }, { "linked_to", "clp-bass" } }));
    CHECK (linked.starts_with ("#2 added clip clp-bass-3 on trk-bass at 5.1, linked to clp-bass"));

    const auto overview = client.text ("get_project_overview");
    CHECK (contains (overview, "  - clip clp-bass-2: 3.1 to 5.1 (@length 2bars), 0 notes\n"));
    CHECK (contains (overview, "  - clip clp-bass-3: 5.1 to 7.1 (@length 2bars), 14 notes, file clips/bass.notes (linked with clp-bass)\n"));

    CHECK (textOf (client.call ("make_clip_unique", { { "clip_id", "clp-bass-3" } })) == "#3 made clip clp-bass-3 unique, with its own file clips/bass-3.notes");
    CHECK (textOf (client.call ("move_clip", { { "clip_id", "clp-bass-3" }, { "start", "9.1" } })) == "#4 moved clp-bass-3 to 9.1");
    CHECK (textOf (client.call ("remove_clip", { { "clip_id", "clp-bass-2" } })) == "#5 removed clip clp-bass-2 from trk-bass");

    // With notes: write_clip's notices.
    CHECK (textOf (client.call ("add_clip", { { "track_id", "trk-bass" }, { "start", "13.1" }, { "notes", "@length 1bar\n\n1.1 C2 1/4 v100\n" } }))
               .starts_with ("#6 added clip"));

    // Exactly one kind of content.
    CHECK (contains (textOf (client.call ("add_clip", { { "track_id", "trk-bass" }, { "start", "15.1" } })), "none was given"));
    CHECK (contains (textOf (client.call ("add_clip", { { "track_id", "trk-bass" }, { "start", "15.1" }, { "bars", 1 }, { "copy_of", "clp-bass" } })),
                     "2 were given"));
    CHECK (contains (textOf (client.call ("add_clip", { { "track_id", "trk-bass" }, { "start", "15.1" }, { "bars", 0 } })), "1 to 999"));
}

TEST_CASE ("patch_clip changes notes, and when one isn't there, shows the clip's lines there now")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    const auto patched = client.call ("patch_clip", { { "clip_id", "clp-bass" },
                                                      { "edits", Json::array ({ { { "old", "1.3  Eb2 1/8 v95" }, { "new", "1.3 F2 1/8 v95" } },
                                                                                { { "new", "2.4.4 C2 1/16 v60" } } }) } });
    CHECK_FALSE (failed (patched));
    CHECK (textOf (patched) == "#1 wrote clip clp-bass (15 notes)\nEdit 1 removed 1 note and added 1; edit 2 added 1 note.");
    CHECK (contains (client.call ("read_clip", { { "clip_id", "clp-bass" } }).at ("content")[1].at ("text").get<std::string>(),
                     "1.3      F2    1/8   v95\n"));

    // Found, but at another velocity: which, and the stored line.
    const auto wrong = client.call ("patch_clip", { { "clip_id", "clp-bass" }, { "edits", Json::array ({ { { "old", "1.3.3 G2 1/8 v90" } } }) } });
    CHECK (failed (wrong));
    CHECK (textOf (wrong) == "patch_clip on clp-bass: edit 1: 1.3.3 G2 is in the clip, but with velocity v85, not v90, so nothing was changed. "
                             "Its line is:\n1.3.3    G2    1/8   v85");

    // Not there: the bar's lines now.
    CHECK (contains (textOf (client.call ("patch_clip", { { "clip_id", "clp-bass" }, { "edits", Json::array ({ { { "old", "2.1 G1 1/4 v100" } } }) } })),
                     "edit 1: 2.1 G1 isn't in the clip, so nothing was changed. Its lines in bar 2 now:\n2.1      Ab1   1/4   v100\n"));
    CHECK (fixture.bus.history().size() == 1);

    // The arguments' shape.
    CHECK (textOf (client.call ("patch_clip", { { "clip_id", "clp-bass" }, { "edits", Json::array() } })).starts_with ("patch_clip: 'edits' is empty"));
    CHECK (textOf (client.call ("patch_clip", { { "clip_id", "clp-bass" }, { "edits", Json::array ({ { { "add", "x" } } }) } }))
           == "patch_clip: edit 1: there's no argument 'add'; its arguments are 'old', 'new'");
}

TEST_CASE ("read_clip with bars gives those bars' lines, with the header and @length")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    const auto bar2 = client.call ("read_clip", { { "clip_id", "clp-bass" }, { "bars", "2" } });
    CHECK (contains (textOf (bar2), "The next block is bar 2 of the clip's .notes text, as stored: its header, its @length line, and the lines whose notes "
                                    "start in bar 2. patch_clip can change these notes; write_clip needs the whole text"));
    CHECK (bar2.at ("content")[1].at ("text")
           == "# bass.notes — Milestone 0 demo: a two-bar bass line in C minor\n@length 2bars\n\n"
              "2.1      Ab1   1/4   v100\n2.2      Ab1   1/8   v75\n2.2.3    C2    1/8   v85\n2.3      G1    1/4   v100\n2.4      G1    1/8   v80\n"
              "2.4.3    Bb1   1/8   v90\n");

    CHECK (client.call ("read_clip", { { "clip_id", "clp-bass" }, { "bars", "1-2" } }).at ("content")[1].at ("text") == demoClipText());
    CHECK (textOf (client.call ("read_clip", { { "clip_id", "clp-bass" }, { "bars", "3" } }))
           == "read_clip: 'bars' \"3\" isn't in the clip: the clip has 2 bars, so bars run from 1 to 2");
    CHECK (textOf (client.call ("read_clip", { { "clip_id", "clp-bass" }, { "bars", "one" } })).starts_with ("read_clip: 'bars' is a bar of the clip"));
}

TEST_CASE ("set_song sets the tempo and the time signature as one entry")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");

    CHECK (textOf (client.call ("set_song", { { "tempo", 96.5 }, { "time_signature", "2/4" } })) == "#1 set the tempo to 96.5; set the time signature to 2/4");
    CHECK (fixture.bus.history().size() == 1);
    CHECK (textOf (client.call ("set_song", { { "tempo", 100 } })) == "#2 set the tempo to 100");
    CHECK (textOf (client.call ("set_song")) == "set_song: give 'tempo', 'time_signature', or both");
}

TEST_CASE ("transport: settings in order, its state with no arguments, and the output line")
{
    Fixture fixture;
    auto client = fixture.connectAs ("session-1");
    CHECK (client.text ("transport") == "Transport: there's no audio engine here, so it can't play.");

    ToolTransport transport;
    ToolMeter meter;
    fixture.bus.setTransport (&transport);
    fixture.bus.setOutputMeter (&meter);

    CHECK (client.text ("transport") == "Transport: stopped. Play plays the whole song, 1.1 to 3.1, on a loop, from 1.1.\n"
                                        "Output: nothing has played since Hocket started.");

    CHECK (client.text ("transport", { { "loop_region", "2.1 to 3.1" }, { "action", "play" } })
           == "loop region 2.1 to 3.1 (1 bar), was the whole song; play starts at 2.1\nplaying the loop region 2.1 to 3.1 on a loop");

    // A minute and a half of the one-bar loop at 120 BPM: 45 bars, so 45 complete passes, and a little over.
    transport.playhead = 3840 + 960;
    transport.progress = { true, 90.4, 45 * 3840 + 960 };
    meter.level = { 1.2f, 312 };
    CHECK (client.text ("transport") == "Transport: playing the loop region 2.1 to 3.1, on a loop; the playhead is at 2.2.\n"
                                        "Output since play started: 1 min 30 s, 45 complete passes; peak +1.6 dBFS, 312 samples over full scale, so the mix clips.");

    meter.level = { 0.5f, 0 };
    CHECK (contains (client.text ("get_project_overview"), "Output since play started: 1 min 30 s, 45 complete passes; peak -6.0 dBFS, never over full scale.\n"));

    // A step that fails stops the rest, and says what was done before it.
    const auto refused = client.call ("transport", { { "loop", false }, { "seek", "9.1" }, { "action", "stop" } });
    CHECK (failed (refused));
    CHECK (contains (textOf (refused), "Done before it: loop off, was on"));
    CHECK (transport.playing);

    CHECK (textOf (client.call ("transport", { { "loop_region", "2.1-3.1" } })).starts_with ("transport: 'loop_region' is a start and an end"));
    CHECK (client.text ("transport", { { "loop_region", "whole song" } }).starts_with ("loop region cleared, was 2.1 to 3.1"));

    fixture.bus.setOutputMeter (nullptr);
    fixture.bus.setTransport (nullptr);
}

TEST_CASE ("redo checks who did the undo, and history narrows to one author")
{
    Fixture fixture;
    auto claude = fixture.connectAs ("session-1");
    auto other = fixture.connectAs ("session-2");

    claude.call ("set_song", { { "tempo", 100 } });
    other.call ("set_song", { { "tempo", 110 } });
    REQUIRE_FALSE (failed (other.call ("undo")));

    const auto refused = claude.call ("redo");
    CHECK (failed (refused));
    CHECK (textOf (refused).starts_with ("didn't redo: the most recent undo, of #2 \"set the tempo to 110\", was done by Claude in another session"));
    CHECK_FALSE (failed (claude.call ("redo", { { "force", true } })));

    CHECK (claude.text ("history", { { "author", "you" } }) == "The only entry of yours:\n#1 you: set the tempo to 100");
    CHECK (claude.text ("history", { { "author", "others" } }) == "The only entry by others:\n#2 Claude in another session: set the tempo to 110");
    CHECK (claude.text ("history", { { "author", "Lee" } })
           == "None of the 2 entries is by Lee. The history's authors: Claude in another session, you.");
}

TEST_CASE ("save, list_saves and revert")
{
    Fixture fixture;
    testing::FakeStorage storage;
    fixture.bus.setStorage (&storage);
    auto client = fixture.connectAs ("session-1");

    CHECK (client.text ("list_saves") == "No kept saves yet. Each save keeps the version it replaces, so there's one to go back to after the next save.\n"
                                         "Nothing is unsaved.");

    client.call ("set_song", { { "tempo", 100 } });
    CHECK (contains (client.text ("get_project_overview"), "There are unsaved changes; save writes them.\n"));
    CHECK (client.text ("save", { { "name", "faster" } }) == "saved");
    REQUIRE (storage.saves.size() == 1);
    CHECK (storage.saves[0].name == "faster");

    storage.keep (1, demoProject(), "before the tempo");
    CHECK (contains (client.text ("list_saves"), "1 kept save:\nsave #1, by Lee, "));
    CHECK (contains (client.text ("list_saves"), "\"before the tempo\"\nNothing is unsaved."));

    CHECK (client.text ("revert", { { "save", 1 } }).starts_with ("#2 reverted to save #1, by Lee, "));
    CHECK (fixture.bus.project().tempo == 120.0);
    CHECK (failed (client.call ("revert", { { "save", 7 } })));

    storage.fail = true;
    CHECK (failed (client.call ("save")));

    fixture.bus.setStorage (nullptr);
}
