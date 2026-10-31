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

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };
    const Author localModel { "local-model", "session-2" };
}

TEST_CASE ("write_clip replaces the clip's notes, records who did it, and announces the change")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    const auto result = bus.execute (WriteClip { "clp-a", twoNotes }, claude);

    CHECK (result.ok);
    CHECK (result.message == "#1 wrote clip clp-a (2 notes)");
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, claude, "wrote clip clp-a (2 notes)" } });
    CHECK (log.changes == std::vector<std::string> { "clp-a" });
}

TEST_CASE ("an invalid write_clip changes nothing and lists every problem in the text")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);
    const auto before = bus.project();

    const auto result = bus.execute (WriteClip { "clp-a", "@length 2bars\n\n1.5 C2 1/4 v100\n1.1 H2 1/4 v100\n" }, claude);

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

    CHECK (bus.execute (WriteClip { "clp-x", twoNotes }, claude).message
           == "write_clip: there's no clip 'clp-x'; the clips are 'clp-a', 'clp-b'");
    CHECK (bus.execute (WriteClip { "trk-bass", twoNotes }, claude).message
           == "write_clip: 'trk-bass' is a track ID; write_clip needs a clip ID like 'clp-bass'");
    CHECK (bus.execute (WriteClip { "dev-bass-synth", twoNotes }, claude).message
           == "write_clip: 'dev-bass-synth' is a device ID; write_clip needs a clip ID like 'clp-bass'");
}

TEST_CASE ("write_clip refuses an @length that would run into the next clip")
{
    CommandBus bus (makeProject());

    const auto tooLong = bus.execute (WriteClip { "clp-a", "@length 5bars\n\n1.1 C2 1/4 v100\n" }, claude);
    CHECK_FALSE (tooLong.ok);
    CHECK (tooLong.message == "write_clip: with @length 5bars, clip 'clp-a' would run from 1.1 to 6.1, into clip 'clp-b' at 5.1. "
                              "Clips on a track can't overlap, so make @length at most 4bars. Nothing was changed");

    CHECK (bus.execute (WriteClip { "clp-a", "@length 4bars\n\n1.1 C2 1/4 v100\n" }, claude).ok);
    CHECK (bus.execute (WriteClip { "clp-b", "@length 8bars\n\n1.1 G1 1/4 v100\n" }, claude).ok); // nothing after it
}

TEST_CASE ("a write that changes nothing adds no history entry")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    // Different spacing, same notes.
    const auto result = bus.execute (WriteClip { "clp-a", "@length 2bars\n1.1      C2    1/4   v100" }, claude);

    CHECK (result.ok);
    CHECK (result.message == "clip 'clp-a' already has exactly these notes, so nothing changed and no history entry was added");
    CHECK (bus.history().empty());
    CHECK (log.changes.empty());
}

TEST_CASE ("writing back the canonical text of a clip loaded from looser text changes nothing")
{
    // The clip as the loader stores it: parsed from a file that isn't in
    // canonical form. read_clip returns the canonical text, and a client that
    // sends that straight back has changed nothing (SPEC §3.1).
    const std::pair<const char*, const char*> files[] = {
        { "lines out of order", "@length 1bar\n1.2 D2 1/4 v80\n1.1 C2 1/4 v80\n" },
        { "a chord split across lines", "@length 1bar\n1.1 E2 1/4 v80 # third\n1.1 C2 1/4 v80 # root\n" },
        { "a comment above an @length after the notes", "1.1 C2 1/4 v80\n# two beats\n@length 1bar\n" },
    };

    for (const auto& [name, onDisk] : files)
    {
        INFO (name);

        auto project = makeProject();
        project.tracks[0].clips[0].content = parsed (onDisk);
        CommandBus bus (std::move (project));
        ChangeLog log (bus);

        const auto canonical = notes::write (contentOf (bus, "clp-a"));
        const auto result = bus.execute (WriteClip { "clp-a", canonical }, claude);

        CHECK (result.ok);
        CHECK (result.message == "clip 'clp-a' already has exactly these notes, so nothing changed and no history entry was added");
        CHECK (bus.history().empty());
        CHECK (log.changes.empty());
    }
}

TEST_CASE ("commands need an author")
{
    CommandBus bus (makeProject());
    CHECK_FALSE (bus.execute (WriteClip { "clp-a", twoNotes }, Author {}).ok);
    CHECK_FALSE (bus.beginTransaction (Author {}, "x").ok);
}

TEST_CASE ("undo and redo walk the history; a new change clears redo")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.execute (WriteClip { "clp-a", twoNotes }, lee);
    bus.execute (WriteClip { "clp-a", threeNotes }, lee);

    const auto undone = bus.undo (lee, UndoScope::anyAuthor);
    CHECK (undone.ok);
    CHECK (undone.message == "undid #2 \"wrote clip clp-a (3 notes)\" by Lee");
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));

    CHECK (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project() == original);
    CHECK (bus.undo (lee, UndoScope::anyAuthor).message == "nothing to undo: the history is empty");

    CHECK (bus.redo (lee).message == "redid #1 \"wrote clip clp-a (2 notes)\" by Lee");
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
    CHECK (bus.history().front().id == 1);

    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, lee);
    CHECK (bus.redo (lee).message == "nothing to redo");
}

TEST_CASE ("an AI's undo refuses to take back someone else's change unless forced")
{
    CommandBus bus (makeProject());
    bus.execute (WriteClip { "clp-a", twoNotes }, lee);
    const auto afterLee = bus.project();

    const auto refused = bus.undo (claude, UndoScope::callersOwn);
    CHECK_FALSE (refused.ok);
    CHECK (refused.message == "didn't undo: the most recent change, #1 \"wrote clip clp-a (2 notes)\", was made by Lee, not Claude. "
                              "Undo only takes back the most recent change, so this would undo their work. "
                              "Leave it, or undo with force to undo it anyway");
    CHECK (bus.project() == afterLee);
    CHECK (bus.history().size() == 1);

    CHECK (bus.undo (claude, UndoScope::anyAuthor).ok); // force
    CHECK (bus.history().empty());
}

TEST_CASE ("an AI can undo its own change, and the human can undo anyone's")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    CHECK (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project() == original);

    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    CHECK (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project() == original);
}

TEST_CASE ("a transaction becomes one named undo step")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    CHECK (bus.beginTransaction (claude, "wrote bridge").message == "began transaction \"wrote bridge\"");
    CHECK (bus.execute (WriteClip { "clp-a", twoNotes }, claude).message
           == "wrote clip clp-a (2 notes) in your open transaction \"wrote bridge\"; it becomes a history entry when you commit");
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, claude);
    CHECK (bus.history().empty()); // not listed until committed

    CHECK (bus.commitTransaction (claude).message == "#1 wrote bridge");
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, claude, "wrote bridge" } });

    CHECK (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project() == original);

    CHECK (bus.redo (claude).ok);
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
}

TEST_CASE ("rolling back a transaction restores everything it changed and records nothing")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);
    const auto original = bus.project();

    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.execute (WriteClip { "clp-a", threeNotes }, claude);

    CHECK (bus.rollbackTransaction (claude).message == "rolled back \"wrote bridge\"");
    CHECK (bus.project() == original);
    CHECK (bus.history().empty());
    CHECK (log.changes.size() == 4); // two edits, then their two inverses
}

TEST_CASE ("transaction misuse gets clear errors")
{
    CommandBus bus (makeProject());

    CHECK (contains (bus.beginTransaction (claude, "").message, "needs a name"));
    CHECK (bus.commitTransaction (claude).message == "you don't have an open transaction to commit");
    CHECK (bus.rollbackTransaction (claude).message == "you don't have an open transaction to roll back");

    bus.beginTransaction (claude, "wrote bridge");
    CHECK (bus.beginTransaction (claude, "again").message == "you already have an open transaction \"wrote bridge\"; commit or roll it back first");
    CHECK (contains (bus.beginTransaction (lee, "mine").message, "Claude has an open transaction \"wrote bridge\", and only one can be open at a time"));
    CHECK (bus.commitTransaction (lee).message == "you don't have an open transaction to commit");
    CHECK (contains (bus.undo (claude, UndoScope::callersOwn).message, "can't undo while your transaction \"wrote bridge\" is open"));
    CHECK (contains (bus.redo (claude).message, "can't redo while your transaction"));

    CHECK (bus.commitTransaction (claude).message == "committed \"wrote bridge\" with no changes; nothing was added to the history");
}

TEST_CASE ("another author's command splits an open transaction into parts instead of waiting")
{
    CommandBus bus (makeProject());

    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);

    CHECK (bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, lee).message == "#2 wrote clip clp-b (1 note)");

    bus.execute (WriteClip { "clp-a", threeNotes }, claude);
    CHECK (bus.commitTransaction (claude).message == "#3 wrote bridge (part 2)");

    CHECK (bus.history() == std::vector<HistoryEntry> {
        { 3, claude, "wrote bridge (part 2)" },
        { 2, lee, "wrote clip clp-b (1 note)" },
        { 1, claude, "wrote bridge (part 1)" },
    });

    // Every part undoes on its own, in order.
    CHECK (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
}

TEST_CASE ("rolling back a split transaction takes back only the current part")
{
    CommandBus bus (makeProject());

    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, lee);
    bus.execute (WriteClip { "clp-a", threeNotes }, claude);

    const auto result = bus.rollbackTransaction (claude);
    CHECK (contains (result.message, "its earlier parts were already in the history"));
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes)); // part 1 stays
    CHECK (bus.history().size() == 2);
}

TEST_CASE ("undo during someone else's transaction")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);

    // Another AI may not take back Claude's work in progress...
    CHECK (contains (bus.undo (localModel, UndoScope::callersOwn).message,
                     "the most recent change is part of the open transaction \"wrote bridge\" by Claude, not yours"));
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));

    // ...but the human can: the part so far is closed and undone.
    const auto undone = bus.undo (lee, UndoScope::anyAuthor);
    CHECK (undone.message == "undid #1 \"wrote bridge (part 1)\" by Claude");
    CHECK (bus.project() == original);

    // Claude carries on in part 2.
    bus.execute (WriteClip { "clp-a", threeNotes }, claude);
    CHECK (bus.commitTransaction (claude).message == "#2 wrote bridge (part 2)");
}

TEST_CASE ("authors are compared by name and session, and messages say which one made a change")
{
    const Author claudeAgain { "Claude", "session-1", "drums" }; // same session reconnecting; the label isn't compared
    const Author otherClaude { "Claude", "session-9" };

    SUBCASE ("the same name in another session is another author")
    {
        CommandBus bus (makeProject());
        bus.execute (WriteClip { "clp-a", twoNotes }, otherClaude);

        CHECK (bus.undo (claude, UndoScope::callersOwn).message
               == "didn't undo: the most recent change, #1 \"wrote clip clp-a (2 notes)\", was made by Claude in another session, not this one. "
                  "Undo only takes back the most recent change, so this would undo their work. Leave it, or undo with force to undo it anyway");

        bus.beginTransaction (otherClaude, "wrote bridge");
        CHECK (contains (bus.beginTransaction (claude, "mine").message, "Claude in another session has an open transaction \"wrote bridge\""));
    }

    SUBCASE ("labels tell same-named authors apart")
    {
        CommandBus bus (makeProject());
        bus.execute (WriteClip { "clp-a", twoNotes }, Author { "Claude", "session-3", "drums" });

        CHECK (contains (bus.undo (Author { "Claude", "session-4", "mixing" }, UndoScope::callersOwn).message,
                         "was made by Claude (drums), not Claude (mixing)."));
        CHECK (bus.undo (lee, UndoScope::anyAuthor).message == "undid #1 \"wrote clip clp-a (2 notes)\" by Claude (drums)");
    }

    SUBCASE ("a client that reconnects with its session ID can still undo its own change")
    {
        CommandBus bus (makeProject());
        bus.execute (WriteClip { "clp-a", twoNotes }, claude);
        CHECK (bus.undo (claudeAgain, UndoScope::callersOwn).ok);
    }

    SUBCASE ("another session's edit splits the transaction")
    {
        CommandBus bus (makeProject());
        bus.beginTransaction (claude, "wrote bridge");
        bus.execute (WriteClip { "clp-a", twoNotes }, claude);
        bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, otherClaude);
        bus.execute (WriteClip { "clp-a", threeNotes }, claudeAgain);

        CHECK (bus.commitTransaction (claudeAgain).message == "#3 wrote bridge (part 2)");
        CHECK (bus.history() == std::vector<HistoryEntry> {
            { 3, claude, "wrote bridge (part 2)" },
            { 2, otherClaude, "wrote clip clp-b (1 note)" },
            { 1, claude, "wrote bridge (part 1)" },
        });
    }
}

TEST_CASE ("an interrupted transaction keeps its edits as one undoable entry")
{
    CommandBus bus (makeProject());
    const auto original = bus.project();

    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, claude);

    const auto result = bus.interruptTransaction (claude);
    CHECK (result.ok);
    CHECK (result.message == "transaction \"wrote bridge\" was closed; its edits are #1 \"wrote bridge (interrupted)\"");
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, claude, "wrote bridge (interrupted)" } });
    CHECK (contentOf (bus, "clp-a") == parsed (twoNotes));
    CHECK_FALSE (bus.openTransaction().has_value());

    // The same client, reconnected, can undo it.
    CHECK (bus.undo (Author { "Claude", "session-1" }, UndoScope::callersOwn).ok);
    CHECK (bus.project() == original);
}

TEST_CASE ("interrupting an empty transaction just closes it")
{
    CommandBus bus (makeProject());
    bus.beginTransaction (claude, "wrote bridge");

    CHECK (bus.interruptTransaction (claude).message == "transaction \"wrote bridge\" was closed with no edits, so nothing was added to the history");
    CHECK (bus.history().empty());
    CHECK_FALSE (bus.openTransaction().has_value());
    CHECK (bus.beginTransaction (lee, "mine").ok);
}

TEST_CASE ("interrupting a split transaction labels the last part")
{
    CommandBus bus (makeProject());
    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, lee);

    SUBCASE ("with edits in the last part")
    {
        bus.execute (WriteClip { "clp-a", threeNotes }, claude);

        CHECK (bus.interruptTransaction (claude).message
               == "transaction \"wrote bridge\" was closed; its earlier parts are already in the history, and the last part is #3 \"wrote bridge (part 2, interrupted)\"");
        CHECK (bus.history().front() == HistoryEntry { 3, claude, "wrote bridge (part 2, interrupted)" });
    }

    SUBCASE ("with none")
    {
        CHECK (bus.interruptTransaction (claude).message
               == "transaction \"wrote bridge\" was closed; its earlier parts are already in the history, and the last part had no edits");
        CHECK (bus.history().size() == 2);
    }
}

TEST_CASE ("only the transaction's own author can be interrupted")
{
    CommandBus bus (makeProject());
    CHECK (bus.interruptTransaction (claude).message == "Claude has no open transaction to interrupt");

    bus.beginTransaction (claude, "wrote bridge");
    CHECK_FALSE (bus.interruptTransaction (Author { "Claude", "session-9" }).ok);
    CHECK_FALSE (bus.interruptTransaction (lee).ok);
    CHECK (bus.openTransaction().has_value());
}

TEST_CASE ("openTransaction describes the open transaction")
{
    CommandBus bus (makeProject());
    CHECK_FALSE (bus.openTransaction().has_value());

    bus.beginTransaction (claude, "wrote bridge");
    auto open = bus.openTransaction();
    REQUIRE (open);
    CHECK (open->author == claude);
    CHECK (open->name == "wrote bridge");
    CHECK (open->part == 1);
    CHECK_FALSE (open->hasEdits);

    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    CHECK (bus.openTransaction()->hasEdits);

    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, lee);
    open = bus.openTransaction();
    CHECK (open->part == 2);
    CHECK_FALSE (open->hasEdits);
}

TEST_CASE ("undo and redo announce each clip they change")
{
    CommandBus bus (makeProject());
    bus.beginTransaction (claude, "both");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.execute (WriteClip { "clp-b", "@length 1bar\n\n1.1 A1 1/4 v100\n" }, claude);
    bus.commitTransaction (claude);

    ChangeLog log (bus);
    bus.undo (lee, UndoScope::anyAuthor);
    CHECK (log.changes == std::vector<std::string> { "clp-b", "clp-a" }); // inverses run newest first
    bus.redo (lee);
    CHECK (log.changes == std::vector<std::string> { "clp-b", "clp-a", "clp-a", "clp-b" });
}

TEST_CASE ("removed listeners hear nothing more")
{
    CommandBus bus (makeProject());
    std::vector<std::string> heard;
    const auto id = bus.addListener ([&heard] (const Change& change) { heard.push_back (std::get<ClipContentChanged> (change).clipId); });

    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.removeListener (id);
    bus.execute (WriteClip { "clp-a", threeNotes }, claude);

    CHECK (heard == std::vector<std::string> { "clp-a" });
}

namespace
{
    // Records the transport commands it's given.
    struct FakeTransport final : Transport
    {
        Result play() override
        {
            calls.push_back ("play");
            return { true, "playing" };
        }

        Result stop() override
        {
            calls.push_back ("stop");
            return { true, "stopped" };
        }

        std::vector<std::string> calls;
    };
}

TEST_CASE ("transport commands go to the transport and never enter the history")
{
    CommandBus bus (makeProject());
    FakeTransport transport;
    bus.setTransport (&transport);
    ChangeLog log (bus);

    CHECK (bus.execute (Play {}, claude).message == "playing");
    CHECK (bus.execute (Stop {}, lee).message == "stopped");

    CHECK (transport.calls == std::vector<std::string> { "play", "stop" });
    CHECK (bus.history().empty());
    CHECK (log.changes.empty());
    CHECK_FALSE (bus.undo (lee, UndoScope::anyAuthor).ok);
}

TEST_CASE ("a transport command doesn't split another author's transaction")
{
    CommandBus bus (makeProject());
    FakeTransport transport;
    bus.setTransport (&transport);

    bus.beginTransaction (claude, "wrote bridge");
    bus.execute (WriteClip { "clp-a", twoNotes }, claude);
    bus.execute (Play {}, lee);
    bus.execute (WriteClip { "clp-a", threeNotes }, claude);

    CHECK (bus.commitTransaction (claude).message == "#1 wrote bridge");
}

TEST_CASE ("transport commands fail clearly when there's no engine, and still need an author")
{
    CommandBus bus (makeProject());

    const auto result = bus.execute (Play {}, claude);
    CHECK_FALSE (result.ok);
    CHECK (result.message == "play: there's no audio engine running, so playback can't be started or stopped here");

    FakeTransport transport;
    bus.setTransport (&transport);
    CHECK_FALSE (bus.execute (Stop {}, Author {}).ok);
    CHECK (transport.calls.empty());

    bus.setTransport (nullptr);
    CHECK_FALSE (bus.execute (Stop {}, claude).ok);
}
