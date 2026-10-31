// hocket-stress's client against a real API server, running as hocket-headless
// runs it (FakeHocket).

#include "api/Protocol.h"
#include "common/FakeHocket.h"
#include "common/TemporaryFolder.h"
#include "core/Paths.h"
#include "core/project/Project.h"
#include "stress/StressClient.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace hocket;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
    // Clear of hocket-headless's ports and the other API tests'.
    constexpr int testPort = 46340;

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

    notes::Clip parsed (std::string_view text)
    {
        auto result = notes::parse (text);
        REQUIRE (result.clip);
        return std::move (*result.clip);
    }

    // A tool's text blocks, joined with blank lines.
    std::string callText (api::Client& client, const std::string& tool, const Json& arguments)
    {
        const auto response = client.request ("tools/call", { { "name", tool }, { "arguments", arguments } }, 5s);
        REQUIRE (response.status == api::Client::Status::answered);
        REQUIRE_MESSAGE (response.message.contains ("result"), response.message.dump());
        std::string joined;

        for (const auto& block : response.message.at ("result").at ("content"))
            joined += (joined.empty() ? "" : "\n\n") + block.at ("text").get<std::string>();

        return joined;
    }

    const std::string demoClip = "@length 1bar\n\n1.1      C2    1/4   v100\n1.3      G2    1/4   v90\n";
}

TEST_CASE ("a steady rewrite changes only its comment, so the music stays the same")
{
    const auto original = parsed (demoClip);
    const auto first = stress::rewriteText (original, 0, 200, false);
    const auto second = stress::rewriteText (original, 1, 200, false);

    CHECK (first == "# hocket-stress: rewrite 1 of 200\n" + demoClip);
    CHECK (second != first);
    CHECK (parsed (second).notes == original.notes);
}

TEST_CASE ("a varied rewrite moves every other one's notes an octave, down where up won't fit")
{
    const auto original = parsed ("@length 1bar\n\n1.1 C2 1/4 v100\n1.2 G9 1/4 v100\n");

    CHECK (parsed (stress::rewriteText (original, 0, 4, true)).notes == original.notes);

    const auto moved = parsed (stress::rewriteText (original, 1, 4, true)).notes;
    REQUIRE (moved.size() == 2);
    CHECK (moved[0].pitch == original.notes[0].pitch + 12);
    CHECK (moved[1].pitch == original.notes[1].pitch - 12);
}

TEST_CASE ("hocket-stress rewrites a clip in a running Hocket on schedule, then puts it back")
{
    testing::TemporaryFolder folder { testing::unicodeName };
    const auto tokenFile = folder.path / pathFromUtf8 ("api.json");
    testing::FakeHocket hocket (tokenFile, testPort, demoProject());

    api::Client client;
    REQUIRE (stress::connect (client, tokenFile, "stress test", "stress-session") == "");
    CHECK (hocket.logged ("stress test connected (session stress-session)"));

    const auto original = callText (client, "read_clip", { { "clip_id", "clp-bass" } });

    stress::Options options;
    options.count = 20;
    options.perSecond = 100.0;
    const auto report = stress::run (client, options);

    CHECK (report.error == "");
    CHECK (report.sent == 20);
    CHECK (report.applied == 20);
    CHECK (report.failures.empty());
    CHECK (report.replySeconds.size() == 20);
    CHECK (report.restored);

    // Paced: 19 intervals of 10 ms between the first send and the last.
    CHECK (report.seconds >= 0.19);
    CHECK (report.seconds < 1.0);

    // 20 rewrites and the restore, all by the stress test, and the clip as it was.
    const auto history = callText (client, "history", { { "limit", 100 } });
    CHECK (contains (history, "all 21 entries"));
    CHECK (contains (history, "#21 you: wrote clip clp-bass"));
    CHECK (callText (client, "read_clip", { { "clip_id", "clp-bass" } }) == original);

    const auto text = stress::summary (report, options);
    CHECK (contains (text, "Rewrote clp-bass 20 times in "));
    CHECK (contains (text, "All 20 were applied, each a history entry."));
    CHECK (contains (text, "The clip is back as it was"));
}

TEST_CASE ("hocket-stress says what's wrong when it can't start")
{
    testing::TemporaryFolder folder { testing::unicodeName };
    const auto tokenFile = folder.path / pathFromUtf8 ("api.json");
    api::Client client;

    SUBCASE ("Hocket isn't running")
    {
        CHECK (contains (stress::connect (client, tokenFile, "stress test", "s"), "Hocket isn't running"));
    }

    SUBCASE ("there's no such clip")
    {
        testing::FakeHocket hocket (tokenFile, testPort, demoProject());
        REQUIRE (stress::connect (client, tokenFile, "stress test", "s") == "");

        stress::Options options;
        options.clipId = "clp-nope";
        const auto report = stress::run (client, options);
        CHECK (report.sent == 0);
        CHECK (contains (report.error, "couldn't read clip clp-nope: "));
        CHECK (contains (stress::summary (report, options), "couldn't read clip clp-nope"));
    }
}

TEST_CASE ("hocket-stress's summary names the rewrites that weren't applied")
{
    stress::Options options;
    stress::Report report;
    report.sent = 3;
    report.applied = 2;
    report.failures = { "rewrite 2: no answer within 5000 ms" };
    report.replySeconds = { 0.001, 0.002, 5.0 };
    report.seconds = 5.2;
    report.mostBehind = 4.9;

    const auto text = stress::summary (report, options);
    CHECK (contains (text, "2 of 3 were applied. The first that weren't:\n  rewrite 2: no answer within 5000 ms\n"));
    CHECK (contains (text, "Some rewrites went out late"));
    CHECK (contains (text, "The clip couldn't be put back as it was."));
}
