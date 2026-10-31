#include "core/commands/WatchedSaves.h"

#include <doctest/doctest.h>

using namespace hocket;
using namespace hocket::commands;

namespace
{
    project::Project makeProject()
    {
        auto parsed = notes::parse ("@length 2bars\n\n1.1 C2 1/4 v100\n");
        REQUIRE (parsed.clip);

        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::fourOsc } }, {} };
        bass.clips.push_back ({ "clp-bass", "clips/bass.notes", 0, std::move (*parsed.clip) });

        project::Project project;
        project.tracks.push_back (std::move (bass));
        return project;
    }

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1", "m0-demo" };

    const std::string leesEdit = "@length 2bars\n\n1.1 C2 1/4 v100\n1.3 G2 1/4 v90\n";
    const std::string claudesEdit = "@length 2bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n";
}

TEST_CASE ("a save goes in as the person's write_clip, with no warning when nothing else changed the clip")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);

    const auto first = saves.save ("clp-bass", leesEdit);
    CHECK (first.result.ok);
    CHECK (first.result.message == "#1 wrote clip clp-bass (2 notes)");
    CHECK_FALSE (first.warning);
    CHECK (bus.history().front().author == lee);

    // Saving again after your own save is fine too.
    CHECK_FALSE (saves.save ("clp-bass", "@length 2bars\n\n1.1 C2 1/4 v100\n").warning);
}

TEST_CASE ("a save that replaces another author's change says so, and how to get it back")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);

    REQUIRE (bus.execute (WriteClip { "clp-bass", claudesEdit }, claude).ok);
    const auto saved = saves.save ("clp-bass", leesEdit);

    CHECK (saved.result.message == "#2 wrote clip clp-bass (2 notes)");
    REQUIRE (saved.warning);
    CHECK (*saved.warning == "warning: this save replaced changes made to clip clp-bass since the file was last loaded or saved "
                             "(the latest: #1 \"wrote clip clp-bass (2 notes)\" by Claude (m0-demo)). Hocket doesn't write clip files yet, "
                             "so the file didn't have them. Undoing #2 brings them back.");

    // The file and the clip match again.
    CHECK_FALSE (saves.save ("clp-bass", leesEdit + "1.4 C3 1/8 v80\n").warning);
}

TEST_CASE ("a change that was undone isn't replaced by a save")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);

    REQUIRE (bus.execute (WriteClip { "clp-bass", claudesEdit }, claude).ok);
    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);

    CHECK_FALSE (saves.save ("clp-bass", leesEdit).warning);
}

TEST_CASE ("a save that's refused replaces nothing, and the next one still warns")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);
    REQUIRE (bus.execute (WriteClip { "clp-bass", claudesEdit }, claude).ok);

    const auto refused = saves.save ("clp-bass", "@length 2bars\n\n1.5 C2 1/4 v100\n");
    CHECK_FALSE (refused.result.ok);
    CHECK_FALSE (refused.warning);

    const auto saved = saves.save ("clp-bass", leesEdit);
    REQUIRE (saved.warning);
    CHECK (saved.warning->find ("#1 \"wrote clip clp-bass (2 notes)\" by Claude (m0-demo)") != std::string::npos);
}

TEST_CASE ("a save matching the clip as it is replaces nothing")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);
    REQUIRE (bus.execute (WriteClip { "clp-bass", claudesEdit }, claude).ok);

    // The file now has Claude's notes too, so nothing is lost, and they match from here on.
    const auto same = saves.save ("clp-bass", claudesEdit);
    CHECK (same.result.ok);
    CHECK_FALSE (same.warning);
    CHECK_FALSE (saves.save ("clp-bass", leesEdit).warning);
}

TEST_CASE ("a save that splits another author's transaction names the part it replaced")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);

    REQUIRE (bus.beginTransaction (claude, "wrote bridge").ok);
    REQUIRE (bus.execute (WriteClip { "clp-bass", claudesEdit }, claude).ok);

    const auto saved = saves.save ("clp-bass", leesEdit);
    REQUIRE (saved.warning);
    CHECK (saved.warning->find ("(the latest: #1 \"wrote bridge (part 1)\" by Claude (m0-demo))") != std::string::npos);
    CHECK (saved.warning->find ("Undoing #2 brings them back.") != std::string::npos);
}

TEST_CASE ("a save for a clip that doesn't exist is the bus's error, with no warning")
{
    CommandBus bus (makeProject());
    WatchedSaves saves (bus, lee);

    const auto saved = saves.save ("clp-nope", leesEdit);
    CHECK_FALSE (saved.result.ok);
    CHECK_FALSE (saved.warning);
}
