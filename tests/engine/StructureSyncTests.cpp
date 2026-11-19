// Tracks, instruments and clips added, moved and removed by commands reach the
// Edit, matched by ID, and undo takes them back (Milestone 1's step 5); so do
// each track's mix, the tempo and the time signature (step 6). While a loop
// region plays once, the Edit's clips end at its end (step 6, part 4).

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
    void run (CommandBus& bus, engine::EditSync& sync, const Command& command)
    {
        const auto result = bus.execute (command, claude);
        INFO (result.message);
        REQUIRE (result.ok);
        checkEditMatches (sync, bus);
    }

    void undoAll (CommandBus& bus, engine::EditSync& sync)
    {
        while (! bus.history().empty())
        {
            REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
            checkEditMatches (sync, bus);
        }
    }
}

TEST_CASE ("sync: tracks added, removed, moved and renamed reach the Edit, and undo takes them back")
{
    auto kitCatalog = randomSequenceKits();
    CommandBus bus (randomSequenceProject());
    bus.setKitCatalog (&kitCatalog);
    engine::EditSync sync (testEngine(), bus);
    checkEditMatches (sync, bus);

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
        checkEditMatches (sync, bus);
    }
}

TEST_CASE ("sync: instruments added, replaced and removed, always ahead of the track's gain stage")
{
    // checkEditMatches checks the gain stage's place after each: one per
    // track, last of the track's own plugins, directly before Tracktion's
    // volume plugin. A test plugin on the bass, as the mix tests put one,
    // stays ahead of it too.
    auto kitCatalog = randomSequenceKits();
    CommandBus bus (randomSequenceProject());
    bus.setKitCatalog (&kitCatalog);
    engine::EditSync sync (testEngine(), bus);
    SteadyTone::addTo (*tracktion::getAudioTracks (sync.edit())[0]);
    sync.edit().getUndoManager().clearUndoHistory();
    checkEditMatches (sync, bus);

    run (bus, sync, SetInstrument { "trk-bass", {} });
    run (bus, sync, SetInstrument { "trk-lead", Instrument { "hocket:synth", {} } });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:synth", {} } });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:drum-sampler", "electronic" } });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:drum-sampler", "acoustic" } });
    run (bus, sync, SetInstrument { "trk-bass", Instrument { "hocket:synth", {} } });
    run (bus, sync, SetInstrument { "trk-lead", {} });
    run (bus, sync, SetInstrument { "trk-drums", {} });
    run (bus, sync, SetInstrument { "trk-drums", Instrument { "hocket:synth", {} } });

    undoAll (bus, sync);
}

TEST_CASE ("sync: clips added, moved, removed, linked and made unique")
{
    auto kitCatalog = randomSequenceKits();
    CommandBus bus (randomSequenceProject());
    bus.setKitCatalog (&kitCatalog);
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

TEST_CASE ("sync: volume, pan, mute and solo reach each track's gain stage, and one track's solo silences the others")
{
    // A track with a mix of its own from the start.
    auto start = randomSequenceProject();
    start.tracks[1].volume = -6.5;
    start.tracks[1].pan = 30;
    auto kitCatalog = randomSequenceKits();
    CommandBus bus (start);
    bus.setKitCatalog (&kitCatalog);
    engine::EditSync sync (testEngine(), bus);
    checkEditMatches (sync, bus);

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

TEST_CASE ("sync: the tempo moves every clip to keep its beats, a time signature moves none, and undo takes them back")
{
    auto kitCatalog = randomSequenceKits();
    CommandBus bus (randomSequenceProject());
    bus.setKitCatalog (&kitCatalog);
    engine::EditSync sync (testEngine(), bus);
    const auto before = randomSequenceProject();

    run (bus, sync, SetTempo { 90.0, {} });
    run (bus, sync, SetTempo { 141.5, {} });
    run (bus, sync, SetTimeSignature { "2/4" });
    run (bus, sync, SetTempo { 20.0, {} });
    run (bus, sync, SetTimeSignature { "8/8" });
    run (bus, sync, SetTempo { 300.0, {} });

    undoAll (bus, sync);
    CHECK (bus.project() == before);
}

TEST_CASE ("sync: random command sequences keep the Edit matching the model, through undo and redo")
{
    int applied = 0;

    for (std::uint32_t seed = 1; seed <= 20; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        auto kitCatalog = randomSequenceKits();
        CommandBus bus (randomSequenceProject());
        bus.setKitCatalog (&kitCatalog);
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
                applied += bus.execute (randomCommand (rng, bus.project()), claude).ok ? 1 : 0;
            }

            checkEditMatches (sync, bus);
        }

        const auto entries = bus.history().size();
        undoAll (bus, sync);

        for (std::size_t i = 0; i < entries; ++i)
            REQUIRE (bus.redo (lee).ok);

        checkEditMatches (sync, bus);
    }

    MESSAGE ("commands applied: " << applied);
    CHECK (applied >= 200);
}

namespace
{
    // Transport commands, mostly valid: a loop region of one to four bars (now
    // and then not whole bars), loop on and off (more often off), a seek,
    // stop and play.
    Command randomTransportCommand (Rng& rng)
    {
        switch (randomInt (rng, 0, 6))
        {
            case 0:
            case 1:
            {
                const auto start = randomInt (rng, 1, 14);
                const auto end = start + randomInt (rng, 1, 4);
                return SetLoopRegion { std::to_string (start) + ".1", std::to_string (end) + (chance (rng, 20) ? ".2" : ".1") };
            }
            case 2: return ClearLoopRegion {};
            case 3: return SetLooping { chance (rng, 40) };
            case 4: return Seek { std::to_string (randomInt (rng, 1, 16)) + ".1" };
            case 5: return Stop {};
            default: return Play {};
        }
    }
}

TEST_CASE ("sync: random commands during playback, with the loop region and looping changing, keep the Edit matching the model, cut short while a region plays once")
{
    int checkedCutShort = 0; // clips checked while a region played once, reaching past its end
    int stoppedAtTheEnd = 0; // by the end check, after playing on
    int transportApplied = 0;

    for (std::uint32_t seed = 1; seed <= 6; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        FakeAudioDevice device;
        auto kitCatalog = randomSequenceKits();
        CommandBus bus (randomSequenceProject());
        bus.setKitCatalog (&kitCatalog);
        engine::EditSync sync (testEngine(), bus);

        // Fast, so plays reach their end often (until a random tempo).
        REQUIRE (bus.execute (SetTempo { 300.0, {} }, lee).ok);
        REQUIRE (bus.execute (SetLoopRegion { "3.1", "5.1" }, lee).ok);
        REQUIRE (bus.execute (SetLooping { false }, lee).ok);
        REQUIRE (timedPlay (bus, lee).ok);

        for (int step = 0; step < 40; ++step)
        {
            CAPTURE (step);

            if (bus.isPlaying() == false && chance (rng, 50))
            {
                REQUIRE (bus.execute (Play {}, lee).ok); // mostly playing, so most steps are made during playback
            }
            else if (chance (rng, 25))
            {
                transportApplied += bus.execute (randomTransportCommand (rng), lee).ok ? 1 : 0;
            }
            else if (chance (rng, 10))
            {
                (void) bus.undo (lee, UndoScope::anyAuthor);
            }
            else if (chance (rng, 5))
            {
                (void) bus.redo (lee);
            }
            else
            {
                (void) bus.execute (randomCommand (rng, bus.project()), claude);
            }

            checkEditMatches (sync, bus);

            // The playhead moves on, and now and then the message loop runs,
            // where the end check may stop it at the end.
            const auto playing = bus.isPlaying() == true;
            device.play (randomInt (rng, 0, 80) * FakeAudioDevice::blockSize);

            if (chance (rng, 50))
                juce::MessageManager::getInstance()->runDispatchLoopUntil (8);

            stoppedAtTheEnd += playing && bus.isPlaying() == false ? 1 : 0;
            checkEditMatches (sync, bus);

            const auto& settings = bus.transportSettings();
            const auto& project = bus.project();

            if (bus.isPlaying() == true && ! settings.looping && settings.loopRegion)
                for (const auto& track : project.tracks)
                    for (const auto& clip : track.clips)
                        checkedCutShort += project::clipEnd (project, clip) > settings.loopRegion->end ? 1 : 0;
        }
    }

    MESSAGE ("transport commands applied: " << transportApplied << "; clips checked cut short or left out: " << checkedCutShort
                                            << "; plays that reached the end and stopped: " << stoppedAtTheEnd);
    CHECK (transportApplied >= 25);
    CHECK (checkedCutShort >= 70);
    CHECK (stoppedAtTheEnd >= 4);
}
