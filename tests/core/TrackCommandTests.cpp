// The track commands: add, remove, rename, move, and the mix.

#include "BusTesting.h"
#include "core/project/Rules.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    constexpr notes::Ticks bar = 3840;

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    // trk-bass: clp-a (2 bars) at 1.1, clp-b (1 bar) at 5.1. trk-lead: clp-a2,
    // linked to clp-a, at 9.1.
    project::Project makeProject()
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::synth } }, {} };
        project::Track lead { "trk-lead", "Lead", project::TrackType::midi, {}, {} };
        project.notesFiles["clips/a.notes"] = parsedNotes ("@length 2bars\n\n1.1 C2 1/4 v100\n");
        project.notesFiles["clips/b.notes"] = parsedNotes ("@length 1bar\n\n1.1 G1 1/4 v100\n");
        bass.clips.push_back ({ "clp-a", "clips/a.notes", 0 });
        bass.clips.push_back ({ "clp-b", "clips/b.notes", 4 * bar });
        lead.clips.push_back ({ "clp-a2", "clips/a.notes", 8 * bar });
        project.tracks.push_back (std::move (bass));
        project.tracks.push_back (std::move (lead));
        return project;
    }

    std::vector<std::string> trackIds (const CommandBus& bus)
    {
        std::vector<std::string> ids;

        for (const auto& track : bus.project().tracks)
            ids.push_back (track.id);

        return ids;
    }

    const project::Track& trackOf (const CommandBus& bus, std::string_view id)
    {
        const auto* track = project::findTrack (bus.project(), id);
        REQUIRE (track != nullptr);
        return *track;
    }

    // Refused, with this message, changing nothing.
    void checkRefused (CommandBus& bus, const Command& command, const std::string& message)
    {
        const auto before = bus.project();
        const auto entries = bus.history().size();
        const auto result = bus.execute (command, claude);

        CHECK_FALSE (result.ok);
        CHECK (result.message == message);
        CHECK (bus.project() == before);
        CHECK (bus.history().size() == entries);
    }
}

//==============================================================================
TEST_CASE ("add a track: at the end, with an ID from its name, announced and undone")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    const auto result = bus.execute (AddTrack { "midi", "Lead Synth", {}, {}, {} }, claude);
    CHECK (result.message == "#1 added track trk-lead-synth \"Lead Synth\"");
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-bass", "trk-lead", "trk-lead-synth" });
    CHECK (log.take() == std::vector<std::string> { "TrackAdded trk-lead-synth" });

    const auto& track = trackOf (bus, "trk-lead-synth");
    CHECK (track.type == project::TrackType::midi);
    CHECK (track.volume == 0.0);
    CHECK (track.pan == 0);
    CHECK_FALSE (track.mute);
    CHECK_FALSE (track.solo);
    CHECK (track.devices.empty());
    CHECK (track.drumMap.empty());
    checkReadsBack (bus.project());

    // Another with a name making the same ID gets the next number.
    CHECK (bus.execute (AddTrack { "midi", "lead synth!", {}, {}, {} }, claude).message == "#2 added track trk-lead-synth-2 \"lead synth!\"");

    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project() == makeProject());
    CHECK (log.take() == std::vector<std::string> { "TrackAdded trk-lead-synth-2", "TrackRemoved trk-lead-synth-2", "TrackRemoved trk-lead-synth" });

    REQUIRE (bus.redo (claude).ok);
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-bass", "trk-lead", "trk-lead-synth" });
}

TEST_CASE ("add a track: a drums track gets the standard drum map, and the result names its instrument")
{
    CommandBus bus (makeProject());

    const auto result = bus.execute (AddTrack { "drums", "Drums", {}, { "trk-bass", {} }, Instrument { "hocket:drum-sampler", "acoustic" } }, claude);
    CHECK (result.message == "#1 added track trk-drums \"Drums\" before trk-bass. Its instrument is dev-drums-kit, a hocket:drum-sampler "
                             "playing kit 'acoustic'. It has the standard drum map: 47 names on General MIDI's notes, such as kick (36), "
                             "snare (38) and hat-closed (42)");

    CHECK (trackIds (bus) == std::vector<std::string> { "trk-drums", "trk-bass", "trk-lead" });
    const auto& drums = trackOf (bus, "trk-drums");
    CHECK (drums.type == project::TrackType::drums);
    CHECK (drums.drumMap == notes::standardDrumMap());
    CHECK (drums.devices == std::vector<project::Device> { { "dev-drums-kit", project::DeviceType::drumSampler, "acoustic" } });
    checkReadsBack (bus.project());

    // A synth, after a track.
    CHECK (bus.execute (AddTrack { "midi", "Keys", {}, { {}, "trk-drums" }, Instrument { "hocket:synth", {} } }, claude).message
           == "#2 added track trk-keys \"Keys\" after trk-drums. Its instrument is dev-keys-synth, a hocket:synth");
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-drums", "trk-keys", "trk-bass", "trk-lead" });

    // A drums track can have a synth, or nothing.
    CHECK (bus.execute (AddTrack { "drums", "Perc", {}, {}, Instrument { "hocket:synth", {} } }, claude).ok);
    CHECK (bus.execute (AddTrack { "drums", "More perc", {}, {}, {} }, claude).ok);
    checkReadsBack (bus.project());
}

TEST_CASE ("add a track: IDs given or made, and the 64-character limit")
{
    CommandBus bus (makeProject());

    // Given.
    CHECK (bus.execute (AddTrack { "midi", "Pad", "trk-pad-1", {}, {} }, claude).message == "#1 added track trk-pad-1 \"Pad\"");

    // A name with no ASCII letters or digits takes the track's type.
    CHECK (bus.execute (AddTrack { "drums", "ドラム", {}, {}, {} }, claude).message.starts_with ("#2 added track trk-drums \"ドラム\". It has the standard drum map"));
    CHECK (bus.execute (AddTrack { "midi", "—", {}, {}, {} }, claude).message == "#3 added track trk-midi \"—\"");
    CHECK (bus.execute (AddTrack { "midi", "Größe 2", {}, {}, {} }, claude).message == "#4 added track trk-gr-e-2 \"Größe 2\"");

    // A long name is cut to fit, and its device's ID keeps its role.
    const std::string longName (100, 'x');
    REQUIRE (bus.execute (AddTrack { "midi", longName, {}, {}, Instrument { "hocket:synth", {} } }, claude).ok);
    const auto& track = bus.project().tracks.back();
    CHECK (track.id == "trk-" + std::string (60, 'x'));
    REQUIRE (track.devices.size() == 1);
    CHECK (track.devices[0].id.size() <= project::maxIdLength);
    CHECK (track.devices[0].id.ends_with ("-synth"));

    REQUIRE (bus.execute (AddTrack { "midi", longName, {}, {}, Instrument { "hocket:synth", {} } }, claude).ok);
    CHECK (bus.project().tracks.back().id == "trk-" + std::string (58, 'x') + "-2");
    CHECK (bus.project().tracks.back().devices[0].id != track.devices[0].id);
    checkReadsBack (bus.project());
}

TEST_CASE ("add a track: every refusal says what's wrong")
{
    CommandBus bus (makeProject());

    checkRefused (bus, AddTrack { "audio", "Vox", {}, {}, {} }, "add a track: \"audio\" tracks aren't supported yet; so far there are \"midi\" and \"drums\" tracks");
    checkRefused (bus, AddTrack { "midi", "", {}, {}, {} },
                  "add a track: a track's name is one line of text, 1 to 100 bytes with no control characters, e.g. \"Bass\"; this one is empty");
    checkRefused (bus, AddTrack { "midi", "Two\nlines", {}, {}, {} },
                  "add a track: a track's name is one line of text, 1 to 100 bytes with no control characters, e.g. \"Bass\"; this one has a line break");
    checkRefused (bus, AddTrack { "midi", "Pad", "clp-pad", {}, {} }, "add a track: 'clp-pad' is a clip ID; a track ID starts with 'trk-', e.g. 'trk-bass'");
    checkRefused (bus, AddTrack { "midi", "Pad", "trk-bass", {}, {} },
                  "add a track: ID 'trk-bass' is already used by track 'trk-bass' (\"Bass\"); IDs are unique across the project, so choose another, "
                  "or leave the ID out and Hocket will make one from the name");
    checkRefused (bus, AddTrack { "midi", "Pad", {}, { "trk-bass", "trk-lead" }, {} }, "add a track: give one place, before or after a track, not both");
    checkRefused (bus, AddTrack { "midi", "Pad", {}, { "trk-nope", {} }, {} }, "add a track: there's no track 'trk-nope'; the tracks are 'trk-bass', 'trk-lead'");
    checkRefused (bus, AddTrack { "midi", "Pad", {}, { {}, "clp-a" }, {} }, "add a track: 'clp-a' is a clip ID; this needs a track ID like 'trk-bass'");
    checkRefused (bus, AddTrack { "midi", "Pad", {}, { {}, "dev-bass-synth" }, {} },
                  "add a track: 'dev-bass-synth' is a device ID, on track 'trk-bass'; this needs a track ID like 'trk-bass'");

    // Instruments.
    checkRefused (bus, AddTrack { "midi", "Kit", {}, {}, Instrument { "hocket:drum-sampler", "acoustic" } },
                  "add a track: a \"hocket:drum-sampler\" goes on a \"drums\" track, whose drum map names the kit's pieces; this is a \"midi\" track. "
                  "Add it as a \"drums\" track, or use \"hocket:synth\"");
    checkRefused (bus, AddTrack { "drums", "Kit", {}, {}, Instrument { "hocket:drum-sampler", {} } },
                  "add a track: a \"hocket:drum-sampler\" needs a kit, e.g. \"acoustic\"");
    checkRefused (bus, AddTrack { "drums", "Kit", {}, {}, Instrument { "hocket:drum-sampler", "Acoustic Kit" } },
                  "add a track: 'Acoustic Kit' isn't a valid kit name: kit names are lowercase letters and digits in words joined by single hyphens, "
                  "e.g. \"acoustic\"");
    checkRefused (bus, AddTrack { "midi", "Keys", {}, {}, Instrument { "hocket:synth", "warm" } },
                  "add a track: only a \"hocket:drum-sampler\" has a kit; leave the kit out for \"hocket:synth\"");
    checkRefused (bus, AddTrack { "midi", "Keys", {}, {}, Instrument { "vst3:serum", {} } },
                  "add a track: \"vst3:serum\" isn't available; so far there are \"hocket:synth\", Hocket's synth, and \"hocket:drum-sampler\", "
                  "which plays a drum kit");

    // The first track of an empty project.
    CommandBus empty (project::Project {});
    checkRefused (empty, AddTrack { "midi", "Pad", {}, { "trk-x", {} }, {} }, "add a track: there's no track 'trk-x'; the project has no tracks yet");
    CHECK (empty.execute (AddTrack { "midi", "Pad", {}, {}, {} }, claude).ok);
}

//==============================================================================
TEST_CASE ("remove a track: its clips go, a file nothing else uses leaves the project, and undo brings it all back")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    // clips/a.notes stays: clp-a2 on trk-lead uses it. clips/b.notes goes.
    CHECK (bus.execute (RemoveTrack { "trk-bass" }, claude).message == "#1 removed track trk-bass \"Bass\" and its 2 clips");
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-lead" });
    CHECK (bus.project().notesFiles.contains ("clips/a.notes"));
    CHECK_FALSE (bus.project().notesFiles.contains ("clips/b.notes"));
    CHECK (log.take() == std::vector<std::string> { "TrackRemoved trk-bass", "NotesChanged clips/b.notes" });
    CHECK (bus.changesTo ("clips/b.notes").size() == 1);
    checkReadsBack (bus.project());

    CHECK (bus.execute (RemoveTrack { "trk-lead" }, claude).message == "#2 removed track trk-lead \"Lead\" and its clip clp-a2");
    CHECK (bus.project().notesFiles.empty());
    CHECK (bus.execute (AddTrack { "midi", "Empty", {}, {}, {} }, claude).ok);
    CHECK (bus.execute (RemoveTrack { "trk-empty" }, claude).message == "#4 removed track trk-empty \"Empty\"");

    for (int i = 0; i < 4; ++i)
        REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);

    CHECK (bus.project() == makeProject());

    checkRefused (bus, RemoveTrack { "trk-nope" }, "remove a track: there's no track 'trk-nope'; the tracks are 'trk-bass', 'trk-lead'");
}

//==============================================================================
TEST_CASE ("rename a track")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (RenameTrack { "trk-bass", "Sub bass" }, claude).message == "#1 renamed track trk-bass \"Bass\" to \"Sub bass\"");
    CHECK (trackOf (bus, "trk-bass").name == "Sub bass");
    CHECK (log.take() == std::vector<std::string> { "TrackRenamed trk-bass" });
    checkReadsBack (bus.project());

    // The ID never changes (SPEC §4.1).
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-bass", "trk-lead" });

    const auto same = bus.execute (RenameTrack { "trk-bass", "Sub bass" }, claude);
    CHECK (same.ok);
    CHECK (same.message == "trk-bass is already called \"Sub bass\", so nothing changed and no history entry was added");

    checkRefused (bus, RenameTrack { "trk-bass", std::string (101, 'a') },
                  "rename trk-bass: a track's name is one line of text, 1 to 100 bytes with no control characters, e.g. \"Bass\"; this one is 101 bytes long");
    checkRefused (bus, RenameTrack { "trk-nope", "X" }, "rename a track: there's no track 'trk-nope'; the tracks are 'trk-bass', 'trk-lead'");

    REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);
    CHECK (bus.project() == makeProject());
}

//==============================================================================
TEST_CASE ("move a track: before or after another, never by index")
{
    auto project = makeProject();
    project.tracks.push_back ({ "trk-pad", "Pad", project::TrackType::midi, {}, {} });
    CommandBus bus (project);
    ChangeLog log (bus);

    CHECK (bus.execute (MoveTrack { "trk-pad", { "trk-bass", {} } }, claude).message == "#1 moved track trk-pad before trk-bass");
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-pad", "trk-bass", "trk-lead" });
    CHECK (log.take() == std::vector<std::string> { "TrackMoved trk-pad" });

    CHECK (bus.execute (MoveTrack { "trk-pad", { {}, "trk-lead" } }, claude).message == "#2 moved track trk-pad after trk-lead");
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-bass", "trk-lead", "trk-pad" });

    CHECK (bus.execute (MoveTrack { "trk-bass", { {}, "trk-lead" } }, claude).message == "#3 moved track trk-bass after trk-lead");
    CHECK (trackIds (bus) == std::vector<std::string> { "trk-lead", "trk-bass", "trk-pad" });
    checkReadsBack (bus.project());

    // Where it already is.
    const auto same = bus.execute (MoveTrack { "trk-bass", { "trk-pad", {} } }, claude);
    CHECK (same.ok);
    CHECK (same.message == "trk-bass is already before trk-pad, so nothing changed and no history entry was added");

    checkRefused (bus, MoveTrack { "trk-bass", {} }, "move trk-bass: say where, before or after another track; to move it last, put it after the last track");
    checkRefused (bus, MoveTrack { "trk-bass", { "trk-bass", {} } }, "move trk-bass: a track can't go before itself");
    checkRefused (bus, MoveTrack { "trk-bass", { "trk-pad", "trk-lead" } }, "move trk-bass: give one place, before or after a track, not both");
    checkRefused (bus, MoveTrack { "trk-bass", { "trk-x", {} } }, "move trk-bass: there's no track 'trk-x'; the tracks are 'trk-lead', 'trk-bass', 'trk-pad'");
    checkRefused (bus, MoveTrack { "trk-x", { "trk-bass", {} } }, "move a track: there's no track 'trk-x'; the tracks are 'trk-lead', 'trk-bass', 'trk-pad'");

    for (int i = 0; i < 3; ++i)
        REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);

    CHECK (bus.project() == project);
}

//==============================================================================
TEST_CASE ("set a track's mix: any of volume, pan, mute and solo, in the format's units")
{
    CommandBus bus (makeProject());
    ChangeLog log (bus);

    CHECK (bus.execute (SetMix { "trk-bass", "-6dB", {}, {}, {} }, claude).message == "#1 set trk-bass's volume to -6dB");
    CHECK (trackOf (bus, "trk-bass").volume == -6.0);
    CHECK (log.take() == std::vector<std::string> { "MixChanged trk-bass" });

    CHECK (bus.execute (SetMix { "trk-bass", "-6 dB", "l20", {}, {} }, claude).message
           == "set trk-bass's mix: 'l20' isn't a pan position: write it in capitals: 'L20'. Pan is 'C' (centre), 'L1' to 'L100' (left) "
              "or 'R1' to 'R100' (right), e.g. 'L20'");
    CHECK (bus.execute (SetMix { "trk-bass", "-6.04dB", "L20", {}, {} }, claude).message == "#2 set trk-bass's pan to L20");
    CHECK (bus.execute (SetMix { "trk-bass", "+3dB", "C", true, {} }, claude).message == "#3 set trk-bass's volume to +3dB, pan to C and mute on");
    CHECK (bus.execute (SetMix { "trk-bass", {}, {}, false, {} }, claude).message == "#4 unmuted trk-bass");
    CHECK (bus.execute (SetMix { "trk-bass", {}, {}, {}, true }, claude).message == "#5 soloed trk-bass");
    CHECK (bus.execute (SetMix { "trk-bass", {}, {}, true, false }, claude).message == "#6 set trk-bass's mute on and solo off");
    CHECK (bus.execute (SetMix { "trk-bass", {}, {}, false, {} }, claude).message == "#7 unmuted trk-bass");
    CHECK (bus.execute (SetMix { "trk-lead", "-infdB", "R100", {}, {} }, claude).message == "#8 set trk-lead's volume to -infdB and pan to R100");
    checkReadsBack (bus.project());

    const auto same = bus.execute (SetMix { "trk-bass", "+3dB", {}, false, false }, claude);
    CHECK (same.ok);
    CHECK (same.message == "trk-bass's mix already has those settings, so nothing changed and no history entry was added");

    checkRefused (bus, SetMix { "trk-bass", {}, {}, {}, {} },
                  "set trk-bass's mix: say what to set: any of its volume (e.g. \"-6dB\"), pan (e.g. \"L20\"), mute and solo");
    checkRefused (bus, SetMix { "trk-bass", "+12dB", {}, {}, {} },
                  "set trk-bass's mix: '+12dB' is out of range: volume goes from -96dB to +6dB, or -infdB, e.g. '-6dB', '0dB' (unchanged), "
                  "'+3dB', '-infdB' (silence)");
    checkRefused (bus, SetMix { "trk-bass", "-6", {}, {}, {} }, "set trk-bass's mix: '-6' isn't a valid volume: it has no unit; volume is written in dB. Write a number "
                                                                 "and a unit, e.g. '-6dB', '0dB' (unchanged), '+3dB', '-infdB' (silence)");
    checkRefused (bus, SetMix { "trk-x", "-6dB", {}, {}, {} }, "set a track's mix: there's no track 'trk-x'; the tracks are 'trk-bass', 'trk-lead'");

    // A refused value changes none of the others.
    checkRefused (bus, SetMix { "trk-bass", "-1dB", "middle", true, {} }, "set trk-bass's mix: 'middle' isn't a pan position: it must be C, or "
                                                                           "a capital L or R and a number. Pan is 'C' (centre), 'L1' to "
                                                                           "'L100' (left) or 'R1' to 'R100' (right), e.g. 'L20'");

    for (int i = 0; i < 8; ++i)
        REQUIRE (bus.undo (claude, UndoScope::callersOwn).ok);

    CHECK (bus.project() == makeProject());
}

//==============================================================================
TEST_CASE ("track commands go into a transaction, and another author's edit splits it")
{
    CommandBus bus (makeProject());
    REQUIRE (bus.beginTransaction (claude, "set up the band").ok);

    CHECK (bus.execute (AddTrack { "drums", "Drums", {}, {}, {} }, claude).message
           == "added track trk-drums \"Drums\" in your open transaction \"set up the band\"; it becomes a history entry when you commit. "
              "It has the standard drum map: 47 names on General MIDI's notes, such as kick (36), snare (38) and hat-closed (42)");

    CHECK (bus.execute (SetMix { "trk-bass", "-3dB", {}, {}, {} }, lee).message == "#2 set trk-bass's volume to -3dB");
    REQUIRE (bus.execute (MoveTrack { "trk-drums", { "trk-bass", {} } }, claude).ok);
    REQUIRE (bus.execute (RemoveTrack { "trk-lead" }, claude).ok);
    CHECK (bus.commitTransaction (claude).message == "#3 set up the band (part 2)");

    CHECK (trackIds (bus) == std::vector<std::string> { "trk-drums", "trk-bass" });
    checkReadsBack (bus.project());

    for (int i = 0; i < 3; ++i)
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

    CHECK (bus.project() == makeProject());

    // Rolled back, it leaves nothing.
    REQUIRE (bus.beginTransaction (claude, "try a pad").ok);
    REQUIRE (bus.execute (AddTrack { "midi", "Pad", {}, {}, {} }, claude).ok);
    REQUIRE (bus.execute (RenameTrack { "trk-pad", "Warm pad" }, claude).ok);
    REQUIRE (bus.rollbackTransaction (claude).ok);
    CHECK (bus.project() == makeProject());
}

//==============================================================================
namespace
{
    // trk-drums: a drum sampler and a small map; clp-beat plays kick and
    // snare2. trk-perc: the same map, and clp-perc linked to clp-beat.
    // trk-bass: a synth.
    project::Project makeDrumProject()
    {
        notes::DrumMap map;
        map.add ("kick", 36);
        map.add ("snare", 38);
        map.add ("clap", 39);
        map.add ("snare2", 40);

        project::Project project;
        project::Track drums { "trk-drums", "Drums", project::TrackType::drums, { { "dev-drums-kit", project::DeviceType::drumSampler, "acoustic" } }, {} };
        project::Track perc { "trk-perc", "Perc", project::TrackType::drums, {}, {} };
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::synth } }, {} };
        drums.drumMap = map;
        perc.drumMap = map;

        project.notesFiles["clips/beat.notes"] = parsedNotes ("@length 1bar\n\n1.1 kick 1/16 v100\n1.2 snare2 1/16 v90\n", {}, &map);
        drums.clips.push_back ({ "clp-beat", "clips/beat.notes", 0 });
        perc.clips.push_back ({ "clp-perc", "clips/beat.notes", 2 * bar });

        project.tracks.push_back (std::move (drums));
        project.tracks.push_back (std::move (perc));
        project.tracks.push_back (std::move (bass));
        return project;
    }

    std::vector<project::DrumMapEntry> entriesOf (const notes::DrumMap& map)
    {
        std::vector<project::DrumMapEntry> entries;

        for (const auto& [note, name] : map.byNote())
            entries.push_back ({ name, note });

        return entries;
    }
}

TEST_CASE ("set a track's instrument: add, replace or remove it, or change only the kit")
{
    CommandBus bus (makeDrumProject());
    ChangeLog log (bus);

    CHECK (bus.execute (SetInstrument { "trk-bass", {} }, claude).message == "#1 removed trk-bass's instrument dev-bass-synth");
    CHECK (trackOf (bus, "trk-bass").devices.empty());
    CHECK (log.take() == std::vector<std::string> { "InstrumentChanged trk-bass" });

    CHECK (bus.execute (SetInstrument { "trk-bass", Instrument { "hocket:synth", {} } }, claude).message
           == "#2 set trk-bass's instrument to dev-bass-synth, a hocket:synth");

    // Only the kit: the device stays.
    CHECK (bus.execute (SetInstrument { "trk-drums", Instrument { "hocket:drum-sampler", "electronic" } }, claude).message
           == "#3 set trk-drums's kit to 'electronic', on dev-drums-kit");
    CHECK (trackOf (bus, "trk-drums").devices
           == std::vector<project::Device> { { "dev-drums-kit", project::DeviceType::drumSampler, "electronic" } });

    // Another kind of device replaces it.
    CHECK (bus.execute (SetInstrument { "trk-drums", Instrument { "hocket:synth", {} } }, claude).message
           == "#4 set trk-drums's instrument to dev-drums-synth, a hocket:synth, replacing dev-drums-kit");
    CHECK (bus.execute (SetInstrument { "trk-perc", Instrument { "hocket:drum-sampler", "jungle-breaks" } }, claude).message
           == "#5 set trk-perc's instrument to dev-perc-kit, a hocket:drum-sampler playing kit 'jungle-breaks'");
    checkReadsBack (bus.project());

    const auto same = bus.execute (SetInstrument { "trk-perc", Instrument { "hocket:drum-sampler", "jungle-breaks" } }, claude);
    CHECK (same.ok);
    CHECK (same.message == "trk-perc's instrument is already dev-perc-kit, a hocket:drum-sampler playing kit 'jungle-breaks', so nothing changed "
                           "and no history entry was added");

    REQUIRE (bus.execute (SetInstrument { "trk-bass", {} }, claude).ok);
    CHECK (bus.execute (SetInstrument { "trk-bass", {} }, claude).message
           == "trk-bass has no instrument already, so nothing changed and no history entry was added");

    checkRefused (bus, SetInstrument { "trk-bass", Instrument { "hocket:drum-sampler", "acoustic" } },
                  "set trk-bass's instrument: a \"hocket:drum-sampler\" goes on a \"drums\" track, whose drum map names the kit's pieces; this is a "
                  "\"midi\" track. A track's type doesn't change, so add a \"drums\" track for a kit, or use \"hocket:synth\"");
    checkRefused (bus, SetInstrument { "trk-perc", Instrument { "hocket:drum-sampler", {} } },
                  "set trk-perc's instrument: a \"hocket:drum-sampler\" needs a kit, e.g. \"acoustic\"");
    checkRefused (bus, SetInstrument { "trk-x", {} }, "set a track's instrument: there's no track 'trk-x'; the tracks are 'trk-drums', 'trk-perc', 'trk-bass'");

    while (! bus.history().empty())
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

    CHECK (bus.project() == makeDrumProject());
}

TEST_CASE ("set a drum map: the notes stay, the names change, and the history says how")
{
    auto project = makeDrumProject();
    project.tracks.erase (project.tracks.begin() + 1); // no trk-perc: clips/beat.notes is trk-drums's alone
    CommandBus bus (project);
    ChangeLog log (bus);

    notes::DrumMap renamed;
    renamed.add ("kick", 36);
    renamed.add ("snare", 38);
    renamed.add ("clap", 39);
    renamed.add ("rim", 40);

    CHECK (bus.execute (SetDrumMap { "trk-drums", entriesOf (renamed) }, claude).message
           == "#1 changed trk-drums's drum map: renamed snare2 to rim. clp-beat is written with the new names now");
    CHECK (bus.project().notesFiles == project.notesFiles); // the notes didn't change
    CHECK (contains (*project::clipFileText (bus.project(), "clips/beat.notes"), "1.2      rim"));
    CHECK (log.take() == std::vector<std::string> { "DrumMapChanged trk-drums" });
    CHECK (bus.changesTo ("clips/beat.notes").size() == 1);
    checkReadsBack (bus.project());

    // Moved, added, removed; nothing the clip uses.
    CHECK (bus.execute (SetDrumMap { "trk-drums", { { "kick", 36 }, { "rim", 40 }, { "clap", 41 }, { "perc", 82 } } }, claude).message
           == "#2 changed trk-drums's drum map: moved clap from 39 to 41, added perc (82) and removed snare (38)");
    CHECK (bus.changesTo ("clips/beat.notes").size() == 1);

    // A note the map no longer names is written as its note name.
    CHECK (bus.execute (SetDrumMap { "trk-drums", { { "kick", 36 } } }, claude).message
           == "#3 changed trk-drums's drum map: removed rim (40), removed clap (41) and removed perc (82). clp-beat is written with the new "
              "names now");
    CHECK (contains (*project::clipFileText (bus.project(), "clips/beat.notes"), "1.2      E2"));
    checkReadsBack (bus.project());

    // A long list of changes is cut short.
    CHECK (bus.execute (SetDrumMap { "trk-drums", entriesOf (notes::standardDrumMap()) }, claude).message
           == "#4 changed trk-drums's drum map: added kick2 (35), added sidestick (37), added snare (38), added clap (39) and 42 more changes. "
              "clp-beat is written with the new names now");

    const auto same = bus.execute (SetDrumMap { "trk-drums", entriesOf (notes::standardDrumMap()) }, claude);
    CHECK (same.ok);
    CHECK (same.message == "trk-drums's drum map is already exactly that, so nothing changed and no history entry was added");

    // An empty map is allowed.
    CHECK (bus.execute (SetDrumMap { "trk-drums", {} }, claude).ok);
    checkReadsBack (bus.project());

    while (! bus.history().empty())
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

    CHECK (bus.project() == project);
}

TEST_CASE ("set a drum map: every refusal says what's wrong")
{
    CommandBus bus (makeDrumProject());

    checkRefused (bus, SetDrumMap { "trk-bass", { { "kick", 36 } } },
                  "set trk-bass's drum map: only \"drums\" tracks have a drum map; trk-bass is a \"midi\" track, whose clips use note names, and a "
                  "track's type doesn't change. To use drum names, add a \"drums\" track and move the clips to it");
    checkRefused (bus, SetDrumMap { "trk-x", {} }, "set a track's drum map: there's no track 'trk-x'; the tracks are 'trk-drums', 'trk-perc', 'trk-bass'");

    // Every problem, as the reader reports them.
    const auto result = bus.execute (SetDrumMap { "trk-drums", { { "kick", 36 }, { "Snare", 38 }, { "rim", 36 }, { "kick", 35 }, { "tom", 200 } } }, claude);
    CHECK_FALSE (result.ok);
    CHECK (result.message.starts_with ("set trk-drums's drum map: 4 problems, so nothing was changed: 'Snare' isn't a valid drum name"));
    CHECK (contains (result.message, "; \"rim\" and \"kick\" are both note 36"));
    CHECK (contains (result.message, "; \"kick\" appears twice"));
    CHECK (contains (result.message, "; \"tom\" should be a MIDI note number from 0 to 127, e.g. 36"));

    // clips/beat.notes is on trk-perc too, which still calls note 40 snare2.
    checkRefused (bus, SetDrumMap { "trk-drums", { { "kick", 36 }, { "snare", 38 }, { "clap", 39 }, { "rim", 40 } } },
                  "set trk-drums's drum map: clips/beat.notes is also used on trk-perc, and with this map the two tracks wouldn't name its notes alike: "
                  "note 40 (E2) is 'rim' on trk-drums but 'snare2' on trk-perc. A file used on several tracks must mean the same notes on each, so "
                  "change both maps alike, or make the clips unique first");

    // Names the file doesn't use may differ.
    CHECK (bus.execute (SetDrumMap { "trk-drums", { { "kick", 36 }, { "snare2", 40 } } }, claude).ok);
    checkReadsBack (bus.project());
}
