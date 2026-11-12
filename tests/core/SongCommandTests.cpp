// The song's commands: tempo and time signature.

#include "BusTesting.h"

#include <limits>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    constexpr notes::Ticks bar = 3840;

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    void addClip (project::Project& project, project::Track& track, std::string id, std::string file, notes::Ticks start, std::string_view text)
    {
        if (! project.notesFiles.contains (file))
            project.notesFiles[file] = parsedNotes (text, project.meter);

        track.clips.push_back ({ std::move (id), std::move (file), start });
    }

    // trk-bass: clp-a (2 bars) at 1.1, clp-b (1 bar) at 5.1. trk-lead: clp-a2,
    // linked to clp-a, at 9.1.
    project::Project makeProject()
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::synth } }, {} };
        project::Track lead { "trk-lead", "Lead", project::TrackType::midi, {}, {} };
        addClip (project, bass, "clp-a", "clips/a.notes", 0, "@length 2bars\n\n1.1 C2 1/4 v100\n2.3 G2 1/4 v90\n");
        addClip (project, bass, "clp-b", "clips/b.notes", 4 * bar, "@length 1bar\n\n1.1 G1 1/4 v100\n");
        addClip (project, lead, "clp-a2", "clips/a.notes", 8 * bar, "");
        project.tracks.push_back (std::move (bass));
        project.tracks.push_back (std::move (lead));
        return project;
    }

    // One track of 3-bar clips at 1.1, 5.1 and, if asked, 9.1.
    project::Project makeThreeBarProject (bool third)
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, {}, {} };
        addClip (project, bass, "clp-a", "clips/a.notes", 0, "@length 3bars\n\n1.1 C2 1/4 v100\n");
        addClip (project, bass, "clp-b", "clips/b.notes", 4 * bar, "@length 3bars\n");

        if (third)
            addClip (project, bass, "clp-c", "clips/c.notes", 8 * bar, "@length 3bars\n");

        project.tracks.push_back (std::move (bass));
        return project;
    }

    std::vector<notes::Ticks> clipStarts (const project::Project& project)
    {
        std::vector<notes::Ticks> starts;

        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                starts.push_back (clip.start);

        return starts;
    }
}

TEST_CASE ("set the tempo: recorded, announced, undone and redone")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    const auto result = bus.execute (SetTempo { 140 }, claude);
    CHECK (result.ok);
    CHECK (result.message == "#1 set the tempo to 140");
    CHECK (bus.project().tempo == 140);
    CHECK (bus.history() == std::vector<HistoryEntry> { { 1, claude, "set the tempo to 140" } });
    CHECK (log.take() == std::vector<std::string> { "TempoChanged" });
    checkReadsBack (bus.project());

    // It changes no clip file.
    CHECK (bus.changesTo ("clips/a.notes").empty());

    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project().tempo == 120);
    CHECK (log.take() == std::vector<std::string> { "TempoChanged" });

    REQUIRE (bus.redo (claude).ok);
    CHECK (bus.project().tempo == 140);
    CHECK (log.take() == std::vector<std::string> { "TempoChanged" });

    CHECK (bus.execute (SetTempo { 97.5 }, lee).message == "#2 set the tempo to 97.5");
    checkReadsBack (bus.project());
}

TEST_CASE ("set the tempo: out of range is refused, and the same tempo changes nothing")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (SetTempo { 1000 }, claude).message
           == "set the tempo: 1000 is out of range: the tempo is 20 to 999 BPM (quarter notes per minute), e.g. 120");

    for (const auto bpm : { 19.99, 999.5, -120.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
    {
        CAPTURE (bpm);
        CHECK_FALSE (bus.execute (SetTempo { bpm }, claude).ok);
    }

    const auto same = bus.execute (SetTempo { 120 }, claude);
    CHECK (same.ok);
    CHECK (same.message == "the tempo is already 120, so nothing changed and no history entry was added");

    CHECK (bus.project() == makeProject());
    CHECK (bus.history().empty());
    CHECK (log.changes.empty());
}

TEST_CASE ("set the time signature: clips and notes keep their times, and @length counts the new bars")
{
    const auto original = makeProject();
    CommandBus bus (original);
    ChangeLog log (bus);

    const auto result = bus.execute (SetTimeSignature { "2/4" }, claude);
    CHECK (result.message == "#1 set the time signature to 2/4");

    const auto& project = bus.project();
    CHECK (project.meter == notes::Meter { 2, 4 });
    CHECK (clipStarts (project) == clipStarts (original));
    CHECK (project.notesFiles.at ("clips/a.notes").lengthInBars == 4);
    CHECK (project.notesFiles.at ("clips/b.notes").lengthInBars == 2);
    CHECK (project.notesFiles.at ("clips/a.notes").notes == original.notesFiles.at ("clips/a.notes").notes);

    // Positions are written in the new bars: 2.3 in 4/4 is 4.1 in 2/4, and bar 5 is bar 9.
    CHECK (contains (*project::clipFileText (project, "clips/a.notes"), "@length 4bars\n\n1.1      C2    1/4   v100\n\n4.1      G2"));
    CHECK (contains (project::write (project).at ("project.json"), "\"id\": \"clp-b\",\n          \"file\": \"clips/b.notes\",\n          \"start\": \"9.1\""));
    checkReadsBack (project);

    CHECK (log.take() == std::vector<std::string> { "TimeSignatureChanged", "NotesChanged clips/a.notes", "NotesChanged clips/b.notes" });
    CHECK (bus.changesTo ("clips/a.notes").size() == 1);
    CHECK (bus.changesTo ("clips/b.notes").size() == 1);

    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project() == original);
    CHECK (log.take().size() == 3);

    REQUIRE (bus.redo (claude).ok);
    CHECK (bus.project().meter == notes::Meter { 2, 4 });
    CHECK (bus.project().notesFiles.at ("clips/a.notes").lengthInBars == 4);
}

TEST_CASE ("set the time signature: 4/4 to 8/8 keeps every @length, and changesTo names the files it writes differently")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (SetTimeSignature { "8/8" }, claude).message == "#1 set the time signature to 8/8");
    CHECK (bus.project().notesFiles.at ("clips/a.notes").lengthInBars == 2);

    // Only the meter changed, but a.notes is written differently now (2.3 is
    // 2.5 in 8/8), while b.notes, with one note at 1.1, reads the same.
    CHECK (log.take() == std::vector<std::string> { "TimeSignatureChanged" });
    CHECK (contains (*project::clipFileText (bus.project(), "clips/a.notes"), "\n2.5      G2"));
    CHECK (bus.changesTo ("clips/a.notes").size() == 1);
    CHECK (bus.changesTo ("clips/b.notes").empty());
    checkReadsBack (bus.project());
}

TEST_CASE ("set the time signature: refused when a clip wouldn't be a whole number of the new bars, saying what would fit")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (SetTimeSignature { "3/4" }, claude).message
           == "set the time signature to 3/4: @length is in bars, and clips keep their length in time, so every clip must be a whole number of "
              "3/4 bars. These aren't: clp-a, clp-a2 (linked): 2bars; clp-b: 1bar. In 4/4, a clip whose @length is a multiple of 3bars fits "
              "(3bars of 4/4 is 4 bars of 3/4), so change those clips' @length first, or choose a signature whose bars they fit. Nothing was changed");

    CHECK (bus.project() == makeProject());
    CHECK (bus.history().empty());
    CHECK (log.changes.empty());

    // 4/4 to 6/8: a 3/4-length bar again.
    CHECK (contains (bus.execute (SetTimeSignature { "6/8" }, claude).message, "In 4/4, a clip whose @length is a multiple of 3bars fits "
                                                                               "(3bars of 4/4 is 4 bars of 6/8)"));
    // 4/4 to 5/4: one clip.
    auto oneClip = makeThreeBarProject (false);
    oneClip.tracks[0].clips.pop_back();
    oneClip.notesFiles.erase ("clips/b.notes");
    CommandBus small (std::move (oneClip));
    CHECK (contains (small.execute (SetTimeSignature { "5/4" }, claude).message, "This one isn't: clp-a: 3bars. In 4/4, a clip whose @length "
                                                                                "is a multiple of 5bars fits (5bars of 4/4 is 4 bars of 5/4)"));
}

TEST_CASE ("set the time signature: when the clips fit, it goes through, and says which clips now start off a bar line")
{
    CommandBus bus (makeThreeBarProject (true));

    // 3 bars of 4/4 are 4 bars of 3/4. Bar 5 of 4/4 is a beat into bar 6 of 3/4, and bar 9 two beats into bar 11.
    CHECK (bus.execute (SetTimeSignature { "3/4" }, claude).message
           == "#1 set the time signature to 3/4. These clips now start off a bar line: clp-b at 6.2 and clp-c at 11.3");
    CHECK (bus.project().notesFiles.at ("clips/a.notes").lengthInBars == 4);
    checkReadsBack (bus.project());

    CommandBus one (makeThreeBarProject (false));
    CHECK (one.execute (SetTimeSignature { "3/4" }, claude).message == "#1 set the time signature to 3/4. clp-b now starts off a bar line, at 6.2");

    // An empty song takes any signature.
    CommandBus empty (project::Project {});
    CHECK (empty.execute (SetTimeSignature { "7/8" }, claude).message == "#1 set the time signature to 7/8");
    checkReadsBack (empty.project());
}

TEST_CASE ("set the time signature: the 999-bar limits apply in the new bars")
{
    project::Project project;
    project::Track bass { "trk-bass", "Bass", project::TrackType::midi, {}, {} };
    addClip (project, bass, "clp-x", "clips/x.notes", 599 * bar, "@length 1bar\n");
    project.tracks.push_back (bass);

    CommandBus late (project);
    CHECK (late.execute (SetTimeSignature { "2/4" }, claude).message
           == "set the time signature to 2/4: a song is at most 999 bars long, so every clip ends by 1000.1, and in 2/4 clp-x would end at 1201.1. "
              "Move it earlier first, or choose a longer bar. Nothing was changed");

    // A 600-bar clip would be 1200 bars of 2/4.
    project.tracks[0].clips.clear();
    project.notesFiles.clear();
    addClip (project, project.tracks[0], "clp-long", "clips/long.notes", 0, "@length 600bars\n");

    CommandBus longClip (project);
    CHECK (longClip.execute (SetTimeSignature { "2/4" }, claude).message
           == "set the time signature to 2/4: a clip is at most 999 bars long, and in 2/4 clp-long would be 1200 bars long. Shorten it first, "
              "or choose a longer bar. Also, a song is at most 999 bars long, so every clip ends by 1000.1, and in 2/4 clp-long would end at "
              "1201.1. Move it earlier first, or choose a longer bar. Nothing was changed");

    // The other way, bars get longer, so it's fine.
    CHECK (longClip.execute (SetTimeSignature { "8/4" }, claude).ok);
    CHECK (longClip.project().notesFiles.at ("clips/long.notes").lengthInBars == 300);
}

TEST_CASE ("set the time signature: an invalid one is refused, and the same one changes nothing")
{
    CommandBus bus (makeProject());

    CHECK (bus.execute (SetTimeSignature { "6/3" }, claude).message
           == "set the time signature: '6/3' isn't a time signature Hocket supports: the bottom number is 3. The top number is 1 to 32 "
              "and the bottom one 2, 4, 8 or 16, e.g. '4/4', '3/4', '6/8', '7/8'");

    const auto same = bus.execute (SetTimeSignature { "4/4" }, claude);
    CHECK (same.ok);
    CHECK (same.message == "the time signature is already 4/4, so nothing changed and no history entry was added");
    CHECK (bus.history().empty());
}

TEST_CASE ("song commands go into a transaction, and another author's edit splits it")
{
    CommandBus bus (makeProject());
    REQUIRE (bus.beginTransaction (claude, "set up the song").ok);

    CHECK (bus.execute (SetTempo { 90 }, claude).message
           == "set the tempo to 90 in your open transaction \"set up the song\"; it becomes a history entry when you commit");

    // Lee's edit comes between, and splits it.
    CHECK (bus.execute (SetTimeSignature { "2/4" }, lee).message == "#2 set the time signature to 2/4");
    CHECK (bus.execute (SetTempo { 100 }, claude).ok);
    CHECK (bus.commitTransaction (claude).message == "#3 set up the song (part 2)");

    CHECK (bus.history() == std::vector<HistoryEntry> { { 3, claude, "set up the song (part 2)" },
                                                        { 2, lee, "set the time signature to 2/4" },
                                                        { 1, claude, "set up the song (part 1)" } });
    CHECK (bus.project().tempo == 100);

    for (int i = 0; i < 3; ++i)
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

    CHECK (bus.project() == makeProject());

    // A remark comes after the transaction's note.
    CommandBus fits (makeThreeBarProject (false));
    REQUIRE (fits.beginTransaction (claude, "waltz").ok);
    CHECK (fits.execute (SetTimeSignature { "3/4" }, claude).message
           == "set the time signature to 3/4 in your open transaction \"waltz\"; it becomes a history entry when you commit. "
              "clp-b now starts off a bar line, at 6.2");

    // Rolling back puts everything back, and announces it.
    ChangeLog log (fits);
    REQUIRE (fits.execute (SetTempo { 80 }, claude).ok);
    REQUIRE (fits.rollbackTransaction (claude).ok);
    CHECK (fits.project() == makeThreeBarProject (false));
    CHECK (log.take() == std::vector<std::string> { "TempoChanged", "TempoChanged", "NotesChanged clips/b.notes", "NotesChanged clips/a.notes",
                                                    "TimeSignatureChanged" });
}
