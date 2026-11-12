// The clip commands: add (empty, from text, linked, a copy), move, remove,
// and make unique.

#include "BusTesting.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    constexpr notes::Ticks bar = 3840;

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    // trk-bass: clp-a (2 bars) at 1.1, clp-b (1 bar) at 5.1. trk-lead: clp-a2,
    // linked to clp-a, at 9.1. trk-drums, with the standard map: clp-beat (1 bar) at 1.1.
    project::Project makeProject()
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, {}, {} };
        project::Track lead { "trk-lead", "Lead", project::TrackType::midi, {}, {} };
        project::Track drums { "trk-drums", "Drums", project::TrackType::drums, {}, {} };
        drums.drumMap = notes::standardDrumMap();

        project.notesFiles["clips/a.notes"] = parsedNotes ("@length 2bars\n\n1.1 C2 1/4 v100\n");
        project.notesFiles["clips/b.notes"] = parsedNotes ("@length 1bar\n\n1.1 G1 1/4 v100\n");
        project.notesFiles["clips/beat.notes"] = parsedNotes ("@length 1bar\n\n1.1 kick 1/16 v100\n", {}, &drums.drumMap);
        bass.clips.push_back ({ "clp-a", "clips/a.notes", 0 });
        bass.clips.push_back ({ "clp-b", "clips/b.notes", 4 * bar });
        lead.clips.push_back ({ "clp-a2", "clips/a.notes", 8 * bar });
        drums.clips.push_back ({ "clp-beat", "clips/beat.notes", 0 });

        project.tracks.push_back (std::move (bass));
        project.tracks.push_back (std::move (lead));
        project.tracks.push_back (std::move (drums));
        return project;
    }

    const project::Clip& clipOf (const CommandBus& bus, std::string_view id)
    {
        const auto* clip = project::findClip (bus.project(), id);
        REQUIRE (clip != nullptr);
        return *clip;
    }

    // Refused, with this message (or starting with it), changing nothing.
    void checkRefused (CommandBus& bus, const Command& command, const std::string& message, bool wholeMessage = true)
    {
        const auto before = bus.project();
        const auto entries = bus.history().size();
        const auto result = bus.execute (command, claude);

        CHECK_FALSE (result.ok);

        if (wholeMessage)
            CHECK (result.message == message);
        else
            CHECK_MESSAGE (result.message.starts_with (message), result.message);

        CHECK (bus.project() == before);
        CHECK (bus.history().size() == entries);
    }

    void undoAll (CommandBus& bus)
    {
        while (! bus.history().empty())
            REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    }
}

//==============================================================================
TEST_CASE ("add a clip: an empty one, with an ID from its track and a file named after it")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (AddClip { "trk-bass", "9.1", EmptyClip { 2 }, {}, {} }, claude).message
           == "#1 added clip clp-bass on trk-bass at 9.1 (empty, 2bars). Its notes are in clips/bass.notes");
    CHECK (log.take() == std::vector<std::string> { "NotesChanged clips/bass.notes", "ClipAdded clp-bass" });
    CHECK (project::clipFileText (bus.project(), "clips/bass.notes") == "@length 2bars\n");

    CHECK (bus.execute (AddClip { "trk-bass", "11.1", EmptyClip {}, {}, {} }, claude).message
           == "#2 added clip clp-bass-2 on trk-bass at 11.1 (empty, 1bar). Its notes are in clips/bass-2.notes");
    checkReadsBack (bus.project());

    // Given an ID and a file.
    CHECK (bus.execute (AddClip { "trk-lead", "1.1", EmptyClip { 4 }, "clp-intro", "clips/lead-intro.notes" }, claude).message
           == "#3 added clip clp-intro on trk-lead at 1.1 (empty, 4bars). Its notes are in clips/lead-intro.notes");
    checkReadsBack (bus.project());

    // Clips stay in start order on their track, as the writer puts them.
    const auto& bass = bus.project().tracks[0].clips;
    REQUIRE (bass.size() == 4);
    CHECK (bass[2].id == "clp-bass");

    undoAll (bus);
    CHECK (bus.project() == makeProject());
}

TEST_CASE ("add a clip: from .notes text, read with the track's drum map")
{
    CommandBus bus (makeProject());

    CHECK (bus.execute (AddClip { "trk-drums", "2.1", ClipText { "@length 1bar\n\n1.1 [kick hat-closed] 1/16 v100\n1.2 snare 1/16 v90\n" }, {}, {} },
                        claude)
               .message
           == "#1 added clip clp-drums on trk-drums at 2.1 (3 notes). Its notes are in clips/drums.notes");
    CHECK (contains (*project::clipFileText (bus.project(), "clips/drums.notes"), "snare"));
    checkReadsBack (bus.project());

    checkRefused (bus, AddClip { "trk-drums", "3.1", ClipText { "1.1 kick 1/16 v100\n1.5 snare 1/16 v90\n" }, {}, {} },
                  "add a clip: the clip's text has 1 error, so nothing was changed:\nclips/drums-2.notes:2:1: '1.5' isn't a valid position", false);
    checkRefused (bus, AddClip { "trk-bass", "9.1", ClipText { "1.1 kick 1/16 v100\n" }, {}, {} }, "add a clip: the clip's text has 1 error", false);
}

TEST_CASE ("add a clip: linked to another, or a copy of one, with IDs from that clip's")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (AddClip { "trk-lead", "13.1", LinkedTo { "clp-a" }, {}, {} }, claude).message
           == "#1 added clip clp-a-2 on trk-lead at 13.1, linked to clp-a");
    CHECK (clipOf (bus, "clp-a-2").file == "clips/a.notes");
    CHECK (log.take() == std::vector<std::string> { "ClipAdded clp-a-2" });

    // Writing one changes all three.
    CHECK (bus.execute (WriteClip { "clp-a", "@length 2bars\n\n1.1 D2 1/4 v100\n" }, claude).message
           == "#2 wrote clip clp-a (1 note), and its linked clips clp-a2, clp-a-2");

    // A copy of clp-a-2 is clp-a-3, with a file of its own and the same notes.
    CHECK (bus.execute (AddClip { "trk-bass", "9.1", CopyOf { "clp-a-2" }, {}, {} }, claude).message
           == "#3 added clip clp-a-3 on trk-bass at 9.1, a copy of clp-a-2. Its notes are in clips/a-3.notes");
    CHECK (bus.project().notesFiles.at ("clips/a-3.notes") == bus.project().notesFiles.at ("clips/a.notes"));
    CHECK (log.take() == std::vector<std::string> { "NotesChanged clips/a.notes", "NotesChanged clips/a-3.notes", "ClipAdded clp-a-3" });

    // Writing the copy leaves the others alone.
    CHECK (bus.execute (WriteClip { "clp-a-3", "@length 2bars\n" }, claude).message == "#4 wrote clip clp-a-3 (0 notes)");
    CHECK (project::notesOf (bus.project(), clipOf (bus, "clp-a")).notes.size() == 1);

    // A copy of a drum clip may go on a midi track: it's written with note names there.
    CHECK (bus.execute (AddClip { "trk-bass", "13.1", CopyOf { "clp-beat" }, "clp-beat-bass", {} }, claude).ok);
    CHECK (contains (*project::clipFileText (bus.project(), "clips/beat-bass.notes"), "C2"));
    checkReadsBack (bus.project());

    undoAll (bus);
    CHECK (bus.project() == makeProject());
}

TEST_CASE ("add a clip: every refusal says what's wrong")
{
    CommandBus bus (makeProject());

    checkRefused (bus, AddClip { "trk-x", "9.1", EmptyClip {}, {}, {} },
                  "add a clip: there's no track 'trk-x'; the tracks are 'trk-bass', 'trk-lead', 'trk-drums'");
    checkRefused (bus, AddClip { "trk-bass", "9.5", EmptyClip {}, {}, {} }, "add a clip: '9.5' isn't a valid position: beat 5 doesn't exist in 4/4", false);
    checkRefused (bus, AddClip { "trk-bass", "9.1", EmptyClip { 0 }, {}, {} }, "add a clip: an empty clip is 1 to 999 bars long, not 0");
    checkRefused (bus, AddClip { "trk-bass", "9.1", LinkedTo { "clp-x" }, {}, {} },
                  "add a clip: there's no clip 'clp-x'; the clips are 'clp-a', 'clp-b', 'clp-a2', 'clp-beat'");
    checkRefused (bus, AddClip { "trk-bass", "9.1", CopyOf { "trk-lead" }, {}, {} },
                  "add a clip: 'trk-lead' is a track ID; this needs a clip ID like 'clp-bass'");
    checkRefused (bus, AddClip { "trk-bass", "9.1", EmptyClip {}, "clp-b", {} },
                  "add a clip: ID 'clp-b' is already used by clip 'clp-b' on trk-bass; IDs are unique across the project, so choose another, or "
                  "leave the ID out and Hocket will make one");
    checkRefused (bus, AddClip { "trk-bass", "9.1", EmptyClip {}, "trk-c", {} },
                  "add a clip: 'trk-c' is a track ID; a clip ID starts with 'clp-', e.g. 'clp-bass-verse'");
    checkRefused (bus, AddClip { "trk-bass", "9.1", EmptyClip {}, {}, "clips/a.notes" },
                  "add a clip: clips/a.notes is already used by clip clp-a; to share a file with it, add the clip linked to it, or name another file");
    checkRefused (bus, AddClip { "trk-bass", "9.1", EmptyClip {}, {}, "clips/My Clip.notes" },
                  "add a clip: 'clips/My Clip.notes' isn't a valid clip file", false);
    checkRefused (bus, AddClip { "trk-bass", "9.1", LinkedTo { "clp-b" }, {}, "clips/c.notes" },
                  "add a clip: a linked clip uses its clip's file, clips/b.notes; leave the file out, or add a copy to give it one of its own");

    // Overlaps, with the nearest room on either side.
    checkRefused (bus, AddClip { "trk-bass", "2.1", EmptyClip { 2 }, {}, {} },
                  "add a clip: it would run from 2.1 to 4.1 on trk-bass, overlapping clip 'clp-a' (1.1 to 3.1); clips on a track can't overlap. "
                  "The nearest start where it fits is 3.1");
    checkRefused (bus, AddClip { "trk-bass", "4.1", EmptyClip { 2 }, {}, {} },
                  "add a clip: it would run from 4.1 to 6.1 on trk-bass, overlapping clip 'clp-b' (5.1 to 6.1); clips on a track can't overlap. "
                  "The nearest starts where it fits are 3.1 and 6.1");
    checkRefused (bus, AddClip { "trk-bass", "2.1", EmptyClip { 4 }, {}, {} },
                  "add a clip: it would run from 2.1 to 6.1 on trk-bass, overlapping clips 'clp-a' (1.1 to 3.1) and 'clp-b' (5.1 to 6.1); clips on a "
                  "track can't overlap. The nearest start where it fits is 6.1");
    checkRefused (bus, AddClip { "trk-bass", "999.1", EmptyClip { 2 }, {}, {} },
                  "add a clip: it runs from 999.1 to 1001.1, but a song is at most 999 bars long, so every clip ends by 1000.1; start it at 998.1 or earlier");

    // A file linked into several tracks must mean the same notes on each.
    checkRefused (bus, AddClip { "trk-bass", "9.1", LinkedTo { "clp-beat" }, {}, {} },
                  "add a clip: clips/beat.notes is used on trk-drums, and the two tracks' drum maps don't name its notes alike: note 36 (C2) is 'kick' "
                  "on trk-drums but trk-bass is a \"midi\" track, with no drum map. A file used on several tracks must mean the same notes on each, so "
                  "make the maps agree, or add a copy instead");
}

//==============================================================================
TEST_CASE ("move a clip: on its track or to another")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (MoveClip { "clp-b", "7.1", {} }, claude).message == "#1 moved clp-b to 7.1");
    CHECK (clipOf (bus, "clp-b").start == 6 * bar);
    CHECK (log.take() == std::vector<std::string> { "ClipMoved clp-b" });

    CHECK (bus.execute (MoveClip { "clp-b", "1.1", "trk-lead" }, claude).message == "#2 moved clp-b to 1.1 on trk-lead");
    CHECK (bus.project().tracks[1].clips.front().id == "clp-b");
    CHECK (log.take() == std::vector<std::string> { "ClipRemoved clp-b from trk-bass", "ClipAdded clp-b" });
    checkReadsBack (bus.project());

    // A clip may move into the space it already takes up.
    CHECK (bus.execute (MoveClip { "clp-a", "2.1", {} }, claude).message == "#3 moved clp-a to 2.1");

    const auto same = bus.execute (MoveClip { "clp-a", "2.1", "trk-bass" }, claude);
    CHECK (same.ok);
    CHECK (same.message == "clp-a is already at 2.1, so nothing changed and no history entry was added");

    undoAll (bus);
    CHECK (bus.project() == makeProject());
}

TEST_CASE ("move a clip: to a track of the other type, its notes are written that track's way")
{
    CommandBus bus (makeProject());

    CHECK (bus.execute (MoveClip { "clp-beat", "9.1", "trk-bass" }, claude).message
           == "#1 moved clp-beat to 9.1 on trk-bass. trk-bass is a \"midi\" track, so its notes are written as note names there");
    CHECK (contains (*project::clipFileText (bus.project(), "clips/beat.notes"), "C2"));
    CHECK (bus.changesTo ("clips/beat.notes").size() == 1);
    checkReadsBack (bus.project());

    CHECK (bus.execute (MoveClip { "clp-beat", "1.1", "trk-drums" }, claude).message
           == "#2 moved clp-beat to 1.1 on trk-drums. trk-drums is a \"drums\" track, so the notes its drum map names are written by name there");
    CHECK (contains (*project::clipFileText (bus.project(), "clips/beat.notes"), "kick"));
}

TEST_CASE ("move a clip: every refusal says what's wrong")
{
    CommandBus bus (makeProject());

    checkRefused (bus, MoveClip { "clp-x", "1.1", {} }, "move a clip: there's no clip 'clp-x'; the clips are 'clp-a', 'clp-b', 'clp-a2', 'clp-beat'");
    checkRefused (bus, MoveClip { "clp-a", "0.1", {} }, "move clp-a: '0.1' isn't a valid position", false);
    checkRefused (bus, MoveClip { "clp-a", "9.1", "trk-x" }, "move clp-a: there's no track 'trk-x'; the tracks are 'trk-bass', 'trk-lead', 'trk-drums'");
    checkRefused (bus, MoveClip { "clp-a", "4.1", {} },
                  "move clp-a: it would run from 4.1 to 6.1 on trk-bass, overlapping clip 'clp-b' (5.1 to 6.1); clips on a track can't overlap. "
                  "The nearest starts where it fits are 3.1 and 6.1");
    checkRefused (bus, MoveClip { "clp-a", "999.1", {} }, "move clp-a: it runs from 999.1 to 1001.1", false);

    // clp-a stays on trk-bass, which has no drum map, so clips/a.notes can't also be on trk-drums.
    checkRefused (bus, MoveClip { "clp-a2", "5.1", "trk-drums" },
                  "move clp-a2: clips/a.notes is used on trk-bass, and the two tracks' drum maps don't name its notes alike: note 36 (C2) is 'kick' on "
                  "trk-drums but trk-bass is a \"midi\" track, with no drum map. A file used on several tracks must mean the same notes on each, so make "
                  "the maps agree, or make the clip unique first");

    // Once unique, it can go.
    REQUIRE (bus.execute (MakeClipUnique { "clp-a2", {} }, claude).ok);
    CHECK (bus.execute (MoveClip { "clp-a2", "5.1", "trk-drums" }, claude).ok);
    checkReadsBack (bus.project());
}

//==============================================================================
TEST_CASE ("remove a clip: a file no clip uses any more leaves the project, and undo brings it back")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (RemoveClip { "clp-b" }, claude).message == "#1 removed clip clp-b from trk-bass");
    CHECK_FALSE (bus.project().notesFiles.contains ("clips/b.notes"));
    CHECK (log.take() == std::vector<std::string> { "ClipRemoved clp-b from trk-bass", "NotesChanged clips/b.notes" });

    // clp-a2 still uses clp-a's file.
    CHECK (bus.execute (RemoveClip { "clp-a" }, claude).message == "#2 removed clip clp-a from trk-bass");
    CHECK (bus.project().notesFiles.contains ("clips/a.notes"));
    CHECK (log.take() == std::vector<std::string> { "ClipRemoved clp-a from trk-bass" });
    checkReadsBack (bus.project());

    checkRefused (bus, RemoveClip { "clp-a" }, "remove a clip: there's no clip 'clp-a'; the clips are 'clp-a2', 'clp-beat'");

    undoAll (bus);
    CHECK (bus.project() == makeProject());
}

//==============================================================================
TEST_CASE ("make a clip unique: it gets a copy of its notes in a file of its own")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (MakeClipUnique { "clp-a2", {} }, claude).message == "#1 made clip clp-a2 unique, with its own file clips/a2.notes");
    CHECK (clipOf (bus, "clp-a2").file == "clips/a2.notes");
    CHECK (bus.project().notesFiles.at ("clips/a2.notes") == bus.project().notesFiles.at ("clips/a.notes"));
    CHECK (log.take() == std::vector<std::string> { "NotesChanged clips/a2.notes", "ClipFileChanged clp-a2" });
    checkReadsBack (bus.project());

    // It's no longer linked.
    CHECK (bus.execute (WriteClip { "clp-a", "@length 2bars\n" }, claude).message == "#2 wrote clip clp-a (0 notes)");
    CHECK (project::notesOf (bus.project(), clipOf (bus, "clp-a2")).notes.size() == 1);

    const auto alone = bus.execute (MakeClipUnique { "clp-a", {} }, claude);
    CHECK (alone.ok);
    CHECK (alone.message == "clp-a isn't linked to another clip: it has clips/a.notes to itself, so nothing changed and no history entry was added");

    undoAll (bus);
    CHECK (bus.project() == makeProject());

    // The clip named after its file: its own name is taken, so the next one.
    CHECK (bus.execute (MakeClipUnique { "clp-a", {} }, claude).message == "#3 made clip clp-a unique, with its own file clips/a-2.notes");

    // A file named in the command.
    undoAll (bus);
    CHECK (bus.execute (MakeClipUnique { "clp-a", "clips/a-verse.notes" }, claude).message == "#4 made clip clp-a unique, with its own file clips/a-verse.notes");

    undoAll (bus);
    checkRefused (bus, MakeClipUnique { "clp-a", "clips/b.notes" },
                  "make clp-a unique: clips/b.notes is already used by clip clp-b; to share a file with it, leave the clip linked, or name another file");
    checkRefused (bus, MakeClipUnique { "clp-x", {} }, "make a clip unique: there's no clip 'clp-x'; the clips are 'clp-a', 'clp-b', 'clp-a2', 'clp-beat'");
}

//==============================================================================
TEST_CASE ("clip commands go into a transaction, and another author's edit splits it")
{
    CommandBus bus (makeProject());
    REQUIRE (bus.beginTransaction (claude, "wrote the chorus").ok);

    CHECK (bus.execute (AddClip { "trk-bass", "9.1", CopyOf { "clp-a" }, {}, {} }, claude).message
           == "added clip clp-a-2 on trk-bass at 9.1, a copy of clp-a in your open transaction \"wrote the chorus\"; it becomes a history entry when "
              "you commit. Its notes are in clips/a-2.notes");
    CHECK (bus.execute (MoveClip { "clp-b", "6.1", {} }, lee).message == "#2 moved clp-b to 6.1");
    REQUIRE (bus.execute (RemoveClip { "clp-a2" }, claude).ok);
    REQUIRE (bus.execute (WriteClip { "clp-a-2", "@length 2bars\n\n1.1 E2 1/2 v90\n" }, claude).ok);
    CHECK (bus.commitTransaction (claude).message == "#3 wrote the chorus (part 2)");
    checkReadsBack (bus.project());

    undoAll (bus);
    CHECK (bus.project() == makeProject());

    REQUIRE (bus.beginTransaction (claude, "try a fill").ok);
    REQUIRE (bus.execute (AddClip { "trk-drums", "2.1", EmptyClip {}, {}, {} }, claude).ok);
    REQUIRE (bus.execute (MoveClip { "clp-drums", "3.1", {} }, claude).ok);
    REQUIRE (bus.rollbackTransaction (claude).ok);
    CHECK (bus.project() == makeProject());
}
