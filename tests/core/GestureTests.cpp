// Gestures (SPEC §3.1): a drag's many commands make one history entry.

#include "BusTesting.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    project::Project makeProject()
    {
        project::Project project;
        project.notesFiles["clips/a.notes"] = parsedNotes ("@length 1bar\n");
        project.tracks.push_back ({ "trk-bass", "Bass", project::TrackType::midi, {}, { { "clp-a", "clips/a.notes", 0 } } });
        project.tracks.push_back ({ "trk-lead", "Lead", project::TrackType::midi, {}, {} });
        return project;
    }

    SetTempo tempo (double bpm, std::string gesture = "drag-1")
    {
        return { bpm, std::move (gesture) };
    }

    SetMix volume (std::string_view track, std::string value, std::string gesture = "drag-1")
    {
        return { std::string (track), std::move (value), {}, {}, {}, std::move (gesture) };
    }

    double volumeOf (const CommandBus& bus, std::string_view track)
    {
        return project::findTrack (bus.project(), track)->volume;
    }
}

TEST_CASE ("a gesture's steps make one history entry, which undoes to the value before the first")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (tempo (121), lee).message == "#1 set the tempo to 121");
    CHECK (bus.execute (tempo (125), lee).message == "#1 set the tempo to 125");
    CHECK (bus.execute (tempo (130), lee).message == "#1 set the tempo to 130");
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, lee, "set the tempo to 130" } });

    // Every step was announced, so the engine follows the drag.
    CHECK (log.take() == std::vector<std::string> { "TempoChanged", "TempoChanged", "TempoChanged" });

    // A step that changes nothing doesn't end it.
    CHECK (bus.execute (tempo (130), lee).ok);
    CHECK (bus.execute (tempo (132), lee).message == "#1 set the tempo to 132");

    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project().tempo == 120);
    REQUIRE (bus.redo (lee).ok);
    CHECK (bus.project().tempo == 132);
    checkReadsBack (bus.project());

    // The mix, the same way.
    CHECK (bus.execute (volume ("trk-bass", "-1dB", "fader"), lee).message == "#2 set trk-bass's volume to -1dB");
    CHECK (bus.execute (volume ("trk-bass", "-3.5dB", "fader"), lee).message == "#2 set trk-bass's volume to -3.5dB");
    CHECK (bus.history().size() == 2);
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (volumeOf (bus, "trk-bass") == 0.0);
}

TEST_CASE ("a gesture that ends where it started leaves no entry")
{
    CommandBus bus (makeProject());
    REQUIRE (bus.execute (tempo (100, {}), claude).ok); // #1, not a gesture

    CHECK (bus.execute (tempo (110), lee).message == "#2 set the tempo to 110");
    CHECK (bus.execute (tempo (100), lee).message
           == "set the tempo to 100; that's where this gesture started, so it leaves no history entry, and #2 is gone");
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, claude, "set the tempo to 100" } });
    CHECK (bus.project().tempo == 100);

    // Numbers are never reused, and the gesture is over.
    CHECK (bus.execute (tempo (105), lee).message == "#3 set the tempo to 105");
    CHECK (bus.execute (tempo (106), lee).message == "#3 set the tempo to 106");
}

TEST_CASE ("undo, redo, another author's edit, another setting or another gesture ends a gesture")
{
    SUBCASE ("undo")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (tempo (110), lee).ok);
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

        CHECK (bus.execute (tempo (115), lee).message == "#2 set the tempo to 115");
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        CHECK (bus.project().tempo == 120);
    }

    SUBCASE ("redo")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (tempo (110), lee).ok);
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        REQUIRE (bus.redo (lee).ok);

        CHECK (bus.execute (tempo (115), lee).message == "#2 set the tempo to 115");
    }

    SUBCASE ("another author's edit")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (tempo (110), lee).ok);
        REQUIRE (bus.execute (WriteClip { "clp-a", "@length 1bar\n\n1.1 C2 1/4 v100\n" }, claude).ok);

        CHECK (bus.execute (tempo (115), lee).message == "#3 set the tempo to 115");
        REQUIRE (bus.undo (lee, UndoScope::callersOwn).ok);
        CHECK (bus.project().tempo == 110); // to where the gesture was when it was interrupted
    }

    SUBCASE ("another author, with the same gesture ID")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (tempo (110), lee).ok);
        CHECK (bus.execute (tempo (115), claude).message == "#2 set the tempo to 115");
    }

    SUBCASE ("another setting")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (volume ("trk-bass", "-1dB"), lee).ok);
        CHECK (bus.execute (volume ("trk-lead", "-2dB"), lee).message == "#2 set trk-lead's volume to -2dB");
        CHECK (bus.execute (SetMix { "trk-lead", "-3dB", "L10", {}, {}, "drag-1" }, lee).message == "#3 set trk-lead's volume to -3dB and pan to L10");
        CHECK (bus.execute (tempo (110), lee).message == "#4 set the tempo to 110");
    }

    SUBCASE ("another gesture ID, or none")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (tempo (110), lee).ok);
        CHECK (bus.execute (tempo (111, "drag-2"), lee).message == "#2 set the tempo to 111");
        CHECK (bus.execute (tempo (112, {}), lee).message == "#3 set the tempo to 112");
        CHECK (bus.execute (tempo (113, {}), lee).message == "#4 set the tempo to 113");
    }
}

TEST_CASE ("a gesture inside a transaction is part of it, and a transaction split by another author ends it")
{
    CommandBus bus (makeProject());
    REQUIRE (bus.beginTransaction (claude, "slowed it down").ok);
    REQUIRE (bus.execute (tempo (110), claude).ok);
    REQUIRE (bus.execute (tempo (100), claude).ok);
    CHECK (bus.commitTransaction (claude).message == "#1 slowed it down");
    CHECK (bus.project().tempo == 100);

    // Lee's gesture, then Claude's transaction splits nothing of Lee's but
    // makes an entry of its own, so Lee's next step is a new entry.
    REQUIRE (bus.execute (volume ("trk-bass", "-1dB"), lee).ok);
    REQUIRE (bus.beginTransaction (claude, "lead level").ok);
    REQUIRE (bus.execute (volume ("trk-lead", "-6dB"), claude).ok);
    REQUIRE (bus.commitTransaction (claude).ok);
    CHECK (bus.execute (volume ("trk-bass", "-2dB"), lee).message == "#4 set trk-bass's volume to -2dB");
    checkReadsBack (bus.project());
}
