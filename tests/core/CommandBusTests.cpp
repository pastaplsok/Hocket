#include "core/commands/CommandBus.h"

#include <doctest/doctest.h>

using namespace hocket;
using namespace hocket::commands;

namespace
{
    constexpr notes::Ticks bar = 3840;

    project::Clip makeClip (std::string id, std::string file, notes::Ticks start, std::string_view notesText)
    {
        auto parsed = notes::parse (notesText);
        REQUIRE (parsed.clip);
        return { std::move (id), std::move (file), start, std::move (*parsed.clip) };
    }

    // One track: clp-a is bars 1-2, clp-b starts at bar 5.
    project::Project makeProject()
    {
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::fourOsc } }, {} };
        bass.clips.push_back (makeClip ("clp-a", "clips/a.notes", 0, "@length 2bars\n\n1.1 C2 1/4 v100\n"));
        bass.clips.push_back (makeClip ("clp-b", "clips/b.notes", 4 * bar, "@length 1bar\n\n1.1 G1 1/4 v100\n"));

        project::Project project;
        project.tracks.push_back (std::move (bass));
        return project;
    }

    const notes::Clip& contentOf (const CommandBus& bus, std::string_view clipId)
    {
        for (const auto& track : bus.project().tracks)
            for (const auto& clip : track.clips)
                if (clip.id == clipId)
                    return clip.content;

        FAIL ("no clip " << clipId);
        throw;
    }

    notes::Clip parsed (std::string_view text)
    {
        auto result = notes::parse (text);
        REQUIRE (result.clip);
        return *result.clip;
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    // Records every change the bus announces, as "clp-a" etc.
    struct ChangeLog
    {
        explicit ChangeLog (CommandBus& bus)
        {
            bus.addListener ([this] (const Change& change) { changes.push_back (std::get<ClipContentChanged> (change).clipId); });
        }

        std::vector<std::string> changes;
    };

    const std::string twoNotes = "@length 2bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n";
    const std::string threeNotes = "@length 2bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n1.3 G2 1/4 v80\n";
}

TEST_CASE ("write_clip replaces the clip's notes, records who did it, and announces the change")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    const auto result = bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");

    CHECK (result.ok);
    CHECK (result.message == "#1 wrote clip clp-a (2 notes)");
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, "Claude", "wrote clip clp-a (2 notes)" } });
    CHECK (log.changes == std::vector<std::string> { "clp-a" });
}

TEST_CASE ("an invalid write_clip changes nothing and lists every problem in the text")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);
    const auto before = bus.project();

    const auto result = bus.execute (WriteClip { "clp-a", "@length 2bars\n\n1.5 C2 1/4 v100\n1.1 H2 1/4 v100\n" }, "Claude");

    CHECK_FALSE (result.ok);
    CHECK (contains (result.message, "write_clip: the new text for clip 'clp-a' has 2 errors, so nothing was changed:"));
    CHECK (contains (result.message, "\nclips/a.notes:3:1: '1.5' isn't a valid position"));
    CHECK (contains (result.message, "\nclips/a.notes:4:5: 'H2' isn't a note name"));
    CHECK (bus.project() == before);
    CHECK (bus.history().empty());
    CHECK (log.changes.empty());
}

TEST_CASE ("write_clip explains an unknown clip ID, including track and device IDs")
{
    CommandBus bus (makeProject());

    CHECK (bus.execute (WriteClip { "clp-x", twoNotes }, "Claude").message
           == "write_clip: there's no clip 'clp-x'; the clips are 'clp-a', 'clp-b'");
    CHECK (bus.execute (WriteClip { "trk-bass", twoNotes }, "Claude").message
           == "write_clip: 'trk-bass' is a track ID; write_clip needs a clip ID like 'clp-bass'");
    CHECK (bus.execute (WriteClip { "dev-bass-synth", twoNotes }, "Claude").message
           == "write_clip: 'dev-bass-synth' is a device ID; write_clip needs a clip ID like 'clp-bass'");
}

TEST_CASE ("write_clip refuses an @length that would run into the next clip")
{
    CommandBus bus (makeProject());

    const auto tooLong = bus.execute (WriteClip { "clp-a", "@length 5bars\n\n1.1 C2 1/4 v100\n" }, "Claude");
    CHECK_FALSE (tooLong.ok);
    CHECK (tooLong.message == "write_clip: with @length 5bars, clip 'clp-a' would run from 1.1 to 6.1, into clip 'clp-b' at 5.1. "
                              "Clips on a track can't overlap, so make @length at most 4bars. Nothing was changed");

    CHECK (bus.execute (WriteClip { "clp-a", "@length 4bars\n\n1.1 C2 1/4 v100\n" }, "Claude").ok);
    CHECK (bus.execute (WriteClip { "clp-b", "@length 8bars\n\n1.1 G1 1/4 v100\n" }, "Claude").ok); // nothing after it
}

TEST_CASE ("a write that changes nothing adds no history entry")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    // Different spacing, same notes.
    const auto result = bus.execute (WriteClip { "clp-a", "@length 2bars\n1.1      C2    1/4   v100" }, "Claude");

    CHECK (result.ok);
    CHECK (result.message == "clip 'clp-a' already has exactly this text; nothing changed");
    CHECK (bus.history().empty());
    CHECK (log.changes.empty());
}

TEST_CASE ("commands need an author")
{
    CommandBus bus (makeProject());
    CHECK_FALSE (bus.execute (WriteClip { "clp-a", twoNotes }, "").ok);
    CHECK_FALSE (bus.beginTransaction ("", "x").ok);
}

TEST_CASE ("undo and redo walk the history; a new change clears redo")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.execute (WriteClip { "clp-a", twoNotes }, "Lee");
    bus.execute (WriteClip { "clp-a", threeNotes }, "Lee");

    const auto undone = bus.undo ("Lee", UndoScope::anyAuthor);
    CHECK (undone.ok);
    CHECK (undone.message == "undid #2 \"wrote clip clp-a (3 notes)\" by Lee");
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));

    CHECK (bus.undo ("Lee", UndoScope::anyAuthor).ok);
    CHECK (bus.project() == original);
    CHECK (bus.undo ("Lee", UndoScope::anyAuthor).message == "nothing to undo: the history is empty");

    CHECK (bus.redo ("Lee").message == "redid #1 \"wrote clip clp-a (2 notes)\" by Lee");
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
    CHECK (bus.history().front().id == 1);

    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, "Lee");
    CHECK (bus.redo ("Lee").message == "nothing to redo");
}

TEST_CASE ("an AI's undo refuses to take back someone else's change unless forced")
{
    CommandBus bus (makeProject());
    bus.execute (WriteClip { "clp-a", twoNotes }, "Lee");
    const auto afterLee = bus.project();

    const auto refused = bus.undo ("Claude", UndoScope::callersOwn);
    CHECK_FALSE (refused.ok);
    CHECK (refused.message == "didn't undo: the most recent change, #1 \"wrote clip clp-a (2 notes)\", was made by Lee, not Claude. "
                              "Undo only takes back the most recent change, so this would undo Lee's work. "
                              "Leave it, or undo with force to undo it anyway");
    CHECK (bus.project() == afterLee);
    CHECK (bus.history().size() == 1);

    CHECK (bus.undo ("Claude", UndoScope::anyAuthor).ok); // force
    CHECK (bus.history().empty());
}

TEST_CASE ("an AI can undo its own change, and the human can undo anyone's")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");
    CHECK (bus.undo ("Claude", UndoScope::callersOwn).ok);
    CHECK (bus.project() == original);

    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");
    CHECK (bus.undo ("Lee", UndoScope::anyAuthor).ok);
    CHECK (bus.project() == original);
}

TEST_CASE ("a transaction becomes one named undo step")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    CHECK (bus.beginTransaction ("Claude", "wrote bridge").message == "began transaction \"wrote bridge\"");
    CHECK (bus.execute (WriteClip { "clp-a", twoNotes }, "Claude").message == "wrote clip clp-a (2 notes) (in transaction \"wrote bridge\")");
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, "Claude");
    CHECK (bus.history().empty()); // not listed until committed

    CHECK (bus.commitTransaction ("Claude").message == "#1 wrote bridge");
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, "Claude", "wrote bridge" } });

    CHECK (bus.undo ("Claude", UndoScope::callersOwn).ok);
    CHECK (bus.project() == original);

    CHECK (bus.redo ("Claude").ok);
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
}

TEST_CASE ("rolling back a transaction restores everything it changed and records nothing")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);
    const auto original = bus.project();

    bus.beginTransaction ("Claude", "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");
    bus.execute (WriteClip { "clp-a", threeNotes }, "Claude");

    CHECK (bus.rollbackTransaction ("Claude").message == "rolled back \"wrote bridge\"");
    CHECK (bus.project() == original);
    CHECK (bus.history().empty());
    CHECK (log.changes.size() == 4); // two edits, then their two inverses
}

TEST_CASE ("transaction misuse gets clear errors")
{
    CommandBus bus (makeProject());

    CHECK (contains (bus.beginTransaction ("Claude", "").message, "needs a name"));
    CHECK (bus.commitTransaction ("Claude").message == "you don't have an open transaction to commit");
    CHECK (bus.rollbackTransaction ("Claude").message == "you don't have an open transaction to roll back");

    bus.beginTransaction ("Claude", "wrote bridge");
    CHECK (bus.beginTransaction ("Claude", "again").message == "you already have an open transaction \"wrote bridge\"; commit or roll it back first");
    CHECK (contains (bus.beginTransaction ("Lee", "mine").message, "Claude has an open transaction \"wrote bridge\", and only one can be open at a time"));
    CHECK (bus.commitTransaction ("Lee").message == "you don't have an open transaction to commit");
    CHECK (contains (bus.undo ("Claude", UndoScope::callersOwn).message, "can't undo while your transaction \"wrote bridge\" is open"));
    CHECK (contains (bus.redo ("Claude").message, "can't redo while your transaction"));

    CHECK (bus.commitTransaction ("Claude").message == "committed \"wrote bridge\" with no changes; nothing was added to the history");
}

TEST_CASE ("another author's command splits an open transaction into parts instead of waiting")
{
    CommandBus bus (makeProject());

    bus.beginTransaction ("Claude", "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");

    CHECK (bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, "Lee").message == "#2 wrote clip clp-b (1 note)");

    bus.execute (WriteClip { "clp-a", threeNotes }, "Claude");
    CHECK (bus.commitTransaction ("Claude").message == "#3 wrote bridge (part 2)");

    CHECK (bus.history() == std::vector<HistoryEntry> {
        { 3, "Claude", "wrote bridge (part 2)" },
        { 2, "Lee", "wrote clip clp-b (1 note)" },
        { 1, "Claude", "wrote bridge (part 1)" },
    });

    // Every part undoes on its own, in order.
    CHECK (bus.undo ("Claude", UndoScope::callersOwn).ok);
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
}

TEST_CASE ("rolling back a split transaction takes back only the current part")
{
    CommandBus bus (makeProject());

    bus.beginTransaction ("Claude", "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, "Lee");
    bus.execute (WriteClip { "clp-a", threeNotes }, "Claude");

    const auto result = bus.rollbackTransaction ("Claude");
    CHECK (contains (result.message, "its earlier parts were already in the history"));
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes)); // part 1 stays
    CHECK (bus.history().size() == 2);
}

TEST_CASE ("undo during someone else's transaction")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.beginTransaction ("Claude", "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");

    // Another AI may not take back Claude's work in progress...
    CHECK (contains (bus.undo ("local-model", UndoScope::callersOwn).message,
                     "the most recent change is part of Claude's open transaction \"wrote bridge\", not yours"));
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));

    // ...but the human can: the part so far is closed and undone.
    const auto undone = bus.undo ("Lee", UndoScope::anyAuthor);
    CHECK (undone.message == "undid #1 \"wrote bridge (part 1)\" by Claude");
    CHECK (bus.project() == original);

    // Claude carries on in part 2.
    bus.execute (WriteClip { "clp-a", threeNotes }, "Claude");
    CHECK (bus.commitTransaction ("Claude").message == "#2 wrote bridge (part 2)");
}

TEST_CASE ("undo and redo announce each clip they change")
{
    CommandBus bus (makeProject());
    bus.beginTransaction ("Claude", "both");
    bus.execute (WriteClip { "clp-a", twoNotes }, "Claude");
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, "Claude");
    bus.commitTransaction ("Claude");

    ChangeLog log (bus);
    bus.undo ("Lee", UndoScope::anyAuthor);
    CHECK (log.changes == std::vector<std::string> { "clp-b", "clp-a" }); // inverses run newest first
    bus.redo ("Lee");
    CHECK (log.changes == std::vector<std::string> { "clp-b", "clp-a", "clp-a", "clp-b" });
}
