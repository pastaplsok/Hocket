// Tracks, instruments and clips added, moved and removed by commands reach the
// Edit, matched by ID, and undo takes them back (Milestone 1's step 5); so does
// each track's mix (step 6).

#include "EngineTesting.h"
#include "common/RandomCommands.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <tuple>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    double beatsOf (notes::Ticks ticks, notes::Meter meter)
    {
        return static_cast<double> (ticks) / static_cast<double> (meter.ticksPerBeat());
    }

    // The Edit's tracks, plugins, clips and notes are the project's, in its order.
    void checkEditMatches (engine::EditSync& sync, const project::Project& project)
    {
        auto& edit = sync.edit();
        const auto meter = project.meter;
        const auto audioTracks = tracktion::getAudioTracks (edit);
        REQUIRE (static_cast<std::size_t> (audioTracks.size()) == project.tracks.size());

        for (std::size_t i = 0; i < project.tracks.size(); ++i)
        {
            const auto& source = project.tracks[i];
            auto& track = *audioTracks[static_cast<int> (i)];
            CAPTURE (source.id);
            CHECK (track.getName() == juce::String::fromUTF8 (source.name.c_str()));

            // A 4OSC for each hocket:synth, first in the chain; nothing for a drum sampler yet.
            const auto synths = std::ranges::count_if (source.devices, [] (const project::Device& d) { return d.type == project::DeviceType::synth; });
            CHECK (track.pluginList.getPluginsOfType<tracktion::FourOscPlugin>().size() == static_cast<int> (synths));

            if (synths > 0)
                CHECK (dynamic_cast<tracktion::FourOscPlugin*> (track.pluginList[0]) != nullptr);

            // The mix, by Tracktion's own sums, is SPEC §4.4's, and isn't automated.
            auto* volume = track.getVolumePlugin();
            REQUIRE (volume != nullptr);
            CHECK (volume->getPanLaw() == tracktion::PanLawLinear);
            CHECK_FALSE (volume->volParam->hasAutomationPoints());
            CHECK_FALSE (volume->panParam->hasAutomationPoints());

            const auto specified = specifiedGains (project, source);
            const auto mixed = volumePluginGains (*volume);
            CAPTURE (source.volume);
            CAPTURE (source.pan);
            CAPTURE (source.mute);
            CAPTURE (source.solo);
            CHECK_MESSAGE (sameGain (mixed.left, specified.left), "left " << mixed.left << ", should be " << specified.left);
            CHECK_MESSAGE (sameGain (mixed.right, specified.right), "right " << mixed.right << ", should be " << specified.right);

            const auto trackClips = track.getClips();
            REQUIRE (static_cast<std::size_t> (trackClips.size()) == source.clips.size());

            for (const auto& clip : source.clips)
            {
                CAPTURE (clip.id);
                const auto found = std::ranges::find_if (trackClips, [&clip] (tracktion::Clip* c) { return c->getName() == juce::String (clip.id); });
                REQUIRE (found != trackClips.end());

                auto* midi = dynamic_cast<tracktion::MidiClip*> (*found);
                REQUIRE (midi != nullptr);
                CHECK (midi->getStartBeat().inBeats() == doctest::Approx (beatsOf (clip.start, meter)));
                CHECK (midi->getEndBeat().inBeats() == doctest::Approx (beatsOf (project::clipEnd (project, clip), meter)));

                using Note = std::tuple<int, double, double, int>; // pitch, start, length, velocity
                std::vector<Note> expected;
                std::vector<Note> actual;

                for (const auto& note : project::notesOf (project, clip).notes)
                    expected.emplace_back (note.pitch, beatsOf (note.start, meter), beatsOf (note.length, meter), note.velocity);

                for (const auto* note : midi->getSequence().getNotes())
                    actual.emplace_back (note->getNoteNumber(), note->getStartBeat().inBeats(), note->getLengthBeats().inBeats(), note->getVelocity());

                std::ranges::sort (expected);
                std::ranges::sort (actual);
                CHECK (actual == expected);
            }
        }

        CHECK (sync.loopRange().getEnd().inSeconds()
               == doctest::Approx (edit.tempoSequence.toTime (tracktion::BeatPosition::fromBeats (beatsOf (project::songEnd (project), meter))).inSeconds()));
        CHECK_FALSE (edit.getUndoManager().canUndo());
    }

    void run (CommandBus& bus, engine::EditSync& sync, const Command& command)
    {
        const auto result = bus.execute (command, claude);
        INFO (result.message);
        REQUIRE (result.ok);
        checkEditMatches (sync, bus.project());
    }

    void undoAll (CommandBus& bus, engine::EditSync& sync)
    {
        while (! bus.history().empty())
        {
            REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
            checkEditMatches (sync, bus.project());
        }
    }
}

TEST_CASE ("sync: tracks added, removed, moved and renamed reach the Edit, and undo takes them back")
{
    CommandBus bus (randomSequenceProject());
    engine::EditSync sync (testEngine(), bus);
    checkEditMatches (sync, bus.project());

    run (bus, sync, AddTrack { "midi", "Keys", {}, { "trk-lead", {} }, Instrument { "hocket:synth", {} } });
    run (bus, sync, AddTrack { "midi", "First", {}, { "trk-bass", {} }, {} });
    run (bus, sync, AddTrack { "drums", "Last", {}, {}, Instrument { "hocket:drum-sampler", "acoustic" } });
    run (bus, sync, RenameTrack { "trk-bass", "Sub bass" });
    run (bus, sync, MoveTrack { "trk-first", { {}, "trk-drums" } });
    run (bus, sync, MoveTrack { "trk-drums", { "trk-bass", {} } });
    run (bus, sync, MoveTrack { "trk-last", { "trk-drums", {} } });
    run (bus, sync, RemoveTrack { "trk-bass" }); // with clp-a and clp-b; clp-a2 on trk-lead keeps clips/a.notes
    run (bus, sync, RemoveTrack { "trk-lead" });
    run (bus, sync, AddClip { "trk-keys", "3.1", CopyOf { "clp-beat" }, {}, {} });

    undoAll (bus, sync);
    CHECK (bus.project() == randomSequenceProject());

    for (int i = 0; i < 10; ++i)
    {
        REQUIRE (bus.redo (lee).ok);
        checkEditMatches (sync, bus.project());
    }
}

TEST_CASE ("sync: instruments added, replaced and removed")
{
    CommandBus bus (randomSequenceProject());
    engine::EditSync sync (testEngine(), bus);

    run (bus, sync, SetInstrument { "trk-bass", {} });
    run (bus, sync, SetInstrument { "trk-lead", Instrument { "hocket:synth", {} } });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:synth", {} } });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:drum-sampler", "electronic" } });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:drum-sampler", "acoustic" } });

    undoAll (bus, sync);
}

TEST_CASE ("sync: clips added, moved, removed, linked and made unique")
{
    CommandBus bus (randomSequenceProject());
    engine::EditSync sync (testEngine(), bus);

    run (bus, sync, AddClip { "trk-bass", "9.1", EmptyClip { 2 }, {}, {} });
    run (bus, sync, AddClip { "trk-lead", "1.1", ClipText { "@length 4bars\n\n1.1 C4 1/2 v80\n3.3 E4 1/8 v70\n" }, {}, {} });
    run (bus, sync, AddClip { "trk-drums", "2.1", LinkedTo { "clp-beat" }, {}, {} });
    run (bus, sync, AddClip { "trk-bass", "13.1", CopyOf { "clp-a" }, {}, {} });
    run (bus, sync, WriteClip { "clp-beat", "@length 1bar\n\n1.1 kick 1/16 v100\n1.3 kick 1/16 v100\n" }); // and its linked clip
    run (bus, sync, MoveClip { "clp-b", "7.1", {} });
    run (bus, sync, MoveClip { "clp-b", "5.1", "trk-lead" });
    run (bus, sync, MoveClip { "clp-a2", "17.1", "trk-bass" });
    run (bus, sync, MakeClipUnique { "clp-a2", {} });
    run (bus, sync, WriteClip { "clp-a2", "@length 2bars\n\n1.1 D2 1/4 v100\n" });
    run (bus, sync, RemoveClip { "clp-a" });
    run (bus, sync, RemoveClip { "clp-beat-2" });

    // The last clip ends the song sooner.
    run (bus, sync, RemoveClip { "clp-a2" });

    undoAll (bus, sync);
}

TEST_CASE ("sync: volume, pan, mute and solo reach each track's volume plugin, and one track's solo silences the others")
{
    // A track with a mix of its own from the start.
    auto start = randomSequenceProject();
    start.tracks[1].volume = -6.5;
    start.tracks[1].pan = 30;
    CommandBus bus (start);
    engine::EditSync sync (testEngine(), bus);
    checkEditMatches (sync, bus.project());

    const auto setMix = [&] (std::string track, std::optional<std::string> volume, std::optional<std::string> pan,
                             std::optional<bool> mute = {}, std::optional<bool> solo = {})
    {
        run (bus, sync, SetMix { std::move (track), std::move (volume), std::move (pan), mute, solo, {} });
    };

    // The ends of each range, and the balance law between them.
    setMix ("trk-bass", "+6dB", "L100");
    setMix ("trk-bass", "-96dB", "R100");
    setMix ("trk-bass", "-96dB", "L1"); // the smallest pan: not rounded to the centre
    setMix ("trk-bass", "-infdB", "R50");
    setMix ("trk-bass", "-0.1dB", "L50");
    setMix ("trk-bass", "0dB", "C");

    // Solo and mute, across tracks.
    setMix ("trk-lead", {}, {}, {}, true);  // only the lead is heard
    setMix ("trk-drums", {}, {}, {}, true); // the lead and the drums
    setMix ("trk-lead", {}, {}, true, {});  // muted, though soloed: only the drums
    setMix ("trk-bass", {}, {}, true, {});  // muted as well as not soloed
    run (bus, sync, RemoveTrack { "trk-drums" }); // the soloed one goes: the lead is soloed, but muted
    run (bus, sync, AddTrack { "midi", "Keys", {}, {}, Instrument { "hocket:synth", {} } }); // silent: the lead is soloed
    setMix ("trk-lead", {}, {}, false, false); // nothing soloed: all but the muted bass

    undoAll (bus, sync);
    CHECK (bus.project() == start);
}

TEST_CASE ("sync: random command sequences keep the Edit matching the model, through undo and redo")
{
    int applied = 0;

    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        CommandBus bus (randomSequenceProject());
        engine::EditSync sync (testEngine(), bus);

        for (int step = 0; step < 30; ++step)
        {
            CAPTURE (step);

            if (chance (rng, 10))
            {
                (void) bus.undo (lee, UndoScope::anyAuthor);
            }
            else if (chance (rng, 5))
            {
                (void) bus.redo (lee);
            }
            else
            {
                // No tempo or time signature: they reach the Edit in step 6.
                applied += bus.execute (randomCommand (rng, bus.project(), false), claude).ok ? 1 : 0;
            }

            checkEditMatches (sync, bus.project());
        }

        const auto entries = bus.history().size();
        undoAll (bus, sync);

        for (std::size_t i = 0; i < entries; ++i)
            REQUIRE (bus.redo (lee).ok);

        checkEditMatches (sync, bus.project());
    }

    MESSAGE ("commands applied: " << applied);
    CHECK (applied >= 200);
}
