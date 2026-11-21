// Step 10's additions to the bus: several commands as one history entry
// (set_track, set_song), redo's author check, and drum map changes (SPEC §6.4).

#include "BusTesting.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    constexpr notes::Ticks bar = 3840;
    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    // trk-drums (kick 36, snare 38, clap 39, snare2 40) with clp-beat; trk-bass with clp-a.
    project::Project makeProject()
    {
        notes::DrumMap map;
        map.add ("kick", 36);
        map.add ("snare", 38);
        map.add ("clap", 39);
        map.add ("snare2", 40);

        project::Project project;
        project::Track drums { "trk-drums", "Drums", project::TrackType::drums, {}, {} };
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::synth } }, {} };
        drums.drumMap = map;

        project.notesFiles["clips/beat.notes"] = parsedNotes ("@length 1bar\n\n1.1 kick 1/16 v100\n1.2 snare2 1/16 v90\n", {}, &map);
        project.notesFiles["clips/a.notes"] = parsedNotes ("@length 2bars\n\n1.1 C2 1/4 v100\n");
        drums.clips.push_back ({ "clp-beat", "clips/beat.notes", 0 });
        bass.clips.push_back ({ "clp-a", "clips/a.notes", 0 });

        project.tracks.push_back (std::move (drums));
        project.tracks.push_back (std::move (bass));
        return project;
    }

    const project::Track& trackOf (const CommandBus& bus, std::string_view id)
    {
        const auto* track = project::findTrack (bus.project(), id);
        REQUIRE (track != nullptr);
        return *track;
    }

    std::string mapText (const notes::DrumMap& map)
    {
        std::string text;

        for (const auto& [note, name] : map.byNote())
            text += (text.empty() ? "" : ", ") + name + "=" + std::to_string (note);

        return text;
    }
}

//==============================================================================
TEST_CASE ("several commands as one: one history entry, undone in one step")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    const auto result = bus.executeAsOne ({ RenameTrack { "trk-bass", "Sub" }, SetMix { "trk-bass", "-6dB", "L20", {}, {} } }, claude);
    CHECK (result.ok);
    CHECK (result.message == "#1 renamed track trk-bass \"Bass\" to \"Sub\"; set trk-bass's volume to -6dB and pan to L20");
    CHECK (bus.history().size() == 1);
    CHECK (trackOf (bus, "trk-bass").name == "Sub");
    CHECK (log.take() == std::vector<std::string> { "TrackRenamed trk-bass", "MixChanged trk-bass" });

    CHECK (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project() == makeProject());
    checkReadsBack (bus.project());
}

TEST_CASE ("several commands as one: each is planned against what the ones before it leave")
{
    CommandBus bus (makeProject());

    // The second only makes sense after the first: its track has the new map.
    const auto result = bus.executeAsOne ({ ChangeDrumMap { "trk-drums", ChangeDrumMap::Base::current, {}, { { "rim", 40 } } },
                                            ChangeDrumMap { "trk-drums", ChangeDrumMap::Base::current, { "rim" }, { { "rimshot", 37 } } } },
                                          claude);
    CHECK (result.ok);
    CHECK (mapText (trackOf (bus, "trk-drums").drumMap) == "kick=36, rimshot=37, snare=38, clap=39");
    CHECK (bus.history().size() == 1);

    // Tempo and time signature together: set_song.
    CHECK (bus.executeAsOne ({ SetTempo { 96.0 }, SetTimeSignature { "2/4" } }, claude).message
           == "#2 set the tempo to 96; set the time signature to 2/4");
    checkReadsBack (bus.project());
}

TEST_CASE ("several commands as one: all or none")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    const auto result = bus.executeAsOne ({ RenameTrack { "trk-bass", "Sub" }, SetMix { "trk-bass", "-200dB", {}, {}, {} } }, claude);
    CHECK_FALSE (result.ok);
    CHECK (result.message.starts_with ("set trk-bass's mix:"));
    CHECK (bus.project() == makeProject());
    CHECK (bus.history().empty());
    CHECK (log.take().empty());
}

TEST_CASE ("several commands as one: unchanged ones are left out, and all unchanged adds nothing")
{
    CommandBus bus (makeProject());

    CHECK (bus.executeAsOne ({ RenameTrack { "trk-bass", "Bass" }, SetMix { "trk-bass", "-3dB", {}, {}, {} } }, claude).message
           == "#1 set trk-bass's volume to -3dB");

    const auto same = bus.executeAsOne ({ RenameTrack { "trk-bass", "Bass" }, SetMix { "trk-bass", "-3dB", {}, {}, {} } }, claude);
    CHECK (same.ok);
    CHECK (same.message == "trk-bass is already called \"Bass\", so nothing changed and no history entry was added; "
                           "trk-bass's mix already has those settings, so nothing changed and no history entry was added");
    CHECK (bus.history().size() == 1);
}

TEST_CASE ("several commands as one: in a transaction, and across another author's")
{
    CommandBus bus (makeProject());

    REQUIRE (bus.beginTransaction (claude, "set up the bass").ok);
    CHECK (bus.executeAsOne ({ RenameTrack { "trk-bass", "Sub" }, SetMix { "trk-bass", "-6dB", {}, {}, {} } }, claude).message
           == "renamed track trk-bass \"Bass\" to \"Sub\"; set trk-bass's volume to -6dB in your open transaction \"set up the bass\"; "
              "it becomes a history entry when you commit");

    // Lee's pair splits it, as a single command would.
    CHECK (bus.executeAsOne ({ SetTempo { 100.0 }, SetTimeSignature { "2/4" } }, lee).message
           == "#2 set the tempo to 100; set the time signature to 2/4");
    CHECK (bus.history()[1].description == "set up the bass (part 1)");
    CHECK (bus.commitTransaction (claude).message == "committed \"set up the bass\"; its earlier parts are already in the history");
}

TEST_CASE ("several commands as one: no transport commands")
{
    CommandBus bus (makeProject());
    const auto result = bus.executeAsOne ({ SetTempo { 100.0 }, Play {} }, claude);
    CHECK_FALSE (result.ok);
    CHECK (contains (result.message, "transport commands don't change the project"));
    CHECK (bus.project() == makeProject());
}

//==============================================================================
TEST_CASE ("redo: refused when someone else did the undo, unless any author's may be redone")
{
    CommandBus bus (makeProject());
    REQUIRE (bus.execute (SetTempo { 100.0 }, claude).ok);
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

    const auto refused = bus.redo (claude, UndoScope::callersOwn);
    CHECK_FALSE (refused.ok);
    CHECK (refused.message == "didn't redo: the most recent undo, of #1 \"set the tempo to 100\", was done by Lee, not Claude, so redoing it would go "
                              "against their undo. Leave it, or redo with force to redo it anyway");
    CHECK (bus.project().tempo == 120.0);

    CHECK (bus.redo (claude, UndoScope::anyAuthor).message == "redid #1 \"set the tempo to 100\" by Claude");
    CHECK (bus.project().tempo == 100.0);

    // Your own undo, you may redo; and another session of the same name is another author.
    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.redo (claude, UndoScope::callersOwn).ok);

    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (contains (bus.redo (Author { "Claude", "session-2" }, UndoScope::callersOwn).message,
                     "was done by Claude in another session, not this one"));

    // The GUI redoes anything.
    CHECK (bus.redo (lee).ok);
}

//==============================================================================
TEST_CASE ("drum map changes: names set and removed, from the track's map")
{
    CommandBus bus (makeProject());

    // Naming a note replaces the name it had.
    CHECK (bus.execute (ChangeDrumMap { "trk-drums", ChangeDrumMap::Base::current, {}, { { "rim", 40 } } }, claude).message
           == "#1 changed trk-drums's drum map: renamed snare2 to rim. clp-beat is written with the new names now");

    // A name set on another note leaves the one it was on; a removed name goes.
    CHECK (bus.execute (ChangeDrumMap { "trk-drums", ChangeDrumMap::Base::current, { "snare" }, { { "clap", 41 } } }, claude).ok);
    CHECK (mapText (trackOf (bus, "trk-drums").drumMap) == "kick=36, rim=40, clap=41");

    // Undone in one step, like any map change.
    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (mapText (trackOf (bus, "trk-drums").drumMap) == "kick=36, snare=38, clap=39, rim=40");
    checkReadsBack (bus.project());
}

TEST_CASE ("drum map changes: from the standard map, or an empty one")
{
    CommandBus bus (makeProject());

    REQUIRE (bus.execute (ChangeDrumMap { "trk-drums", ChangeDrumMap::Base::standard, {}, { { "rim", 37 } } }, claude).ok);
    auto expected = notes::standardDrumMap().byNote();
    expected[37] = "rim";
    CHECK (trackOf (bus, "trk-drums").drumMap.byNote() == expected);

    REQUIRE (bus.execute (ChangeDrumMap { "trk-drums", ChangeDrumMap::Base::empty, {}, { { "kick", 36 }, { "snare2", 40 } } }, claude).ok);
    CHECK (mapText (trackOf (bus, "trk-drums").drumMap) == "kick=36, snare2=40");
    checkReadsBack (bus.project());
}

TEST_CASE ("drum map changes: refusals")
{
    CommandBus bus (makeProject());
    const auto refusal = [&bus] (ChangeDrumMap command) { return bus.execute (command, claude).message; };

    CHECK (refusal ({ "trk-drums", ChangeDrumMap::Base::current, { "snaer" }, {} })
           == "change trk-drums's drum map: there's no \"snaer\" in it to remove; did you mean snare or snare2?");
    CHECK (refusal ({ "trk-drums", ChangeDrumMap::Base::current, {}, { { "Rim", 37 } } }).starts_with ("set trk-drums's drum map: 1 problem"));
    CHECK (refusal ({ "trk-drums", ChangeDrumMap::Base::current, {}, { { "rim", 200 } } }).starts_with ("set trk-drums's drum map: 1 problem"));
    CHECK (contains (refusal ({ "trk-bass", ChangeDrumMap::Base::current, {}, { { "rim", 37 } } }), "only \"drums\" tracks have a drum map"));
    CHECK (contains (refusal ({ "trk-nope", ChangeDrumMap::Base::current, {}, {} }), "trk-nope"));
    CHECK (bus.history().empty());
}
