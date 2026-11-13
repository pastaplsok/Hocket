// Live playback through the fake audio device while tracks and clips are
// added, moved and removed (Milestone 1's step 5).

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // At 120 BPM and 44.1 kHz.
    constexpr int beatSamples = 22050;
    constexpr int barSamples = 4 * beatSamples;

    // Within 10 ms: a little over one block.
    bool nearSample (int sample, int expected)
    {
        return std::abs (sample - expected) < 441;
    }

    // What the app does on the message thread straight after a command.
    void rebuildGraph (engine::EditSync& sync)
    {
        sync.edit().dispatchPendingUpdatesSynchronously();
    }

    void run (commands::CommandBus& bus, engine::EditSync& sync, const commands::Command& command)
    {
        const auto result = bus.execute (command, testing::claude);
        INFO (result.message);
        REQUIRE (result.ok);
        rebuildGraph (sync);
    }

    const std::string wholeBar = "@length 1bar\n\n1.1 C3 1/1 v100\n";
    const std::string shortNote = "@length 1bar\n\n1.1 C3 1/8 v100\n";

    // A whole-bar C3 on the bass at 1.1, and a short note on the lead at 3.1, so the song is three bars.
    project::Project heldNoteProject()
    {
        return makeProject ({
            makeTrack ("trk-bass", "Bass", { makeClip ("clp-held", 0, wholeBar) }),
            makeTrack ("trk-lead", "Lead", { makeClip ("clp-later", 2 * bar, shortNote) }),
        });
    }

    // 4OSC's release is 0.1 s, so whatever a change ends is silent from a
    // quarter of a second after it.
    void checkSilent (const juce::AudioBuffer<float>& out, int from, int to)
    {
        const auto peak = peakBetween (out, from, to);
        CHECK_MESSAGE (peak < 1.0e-3f, "something still sounds between samples " << from << " and " << to << ", at a peak of " << peak);
    }
}

TEST_CASE ("live: a clip removed while its note sounds ends the note")
{
    FakeAudioDevice device;
    commands::CommandBus bus (heldNoteProject());
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (bus.execute (commands::Play {}, testing::lee).ok);

    device.play (beatSamples);
    run (bus, sync, commands::RemoveClip { "clp-held" });
    device.play (2 * barSamples - beatSamples - FakeAudioDevice::blockSize);

    const auto& out = device.output().buffer;
    REQUIRE (out.getRMSLevel (0, beatSamples / 2, beatSamples / 2) > 0.01f);
    checkSilent (out, beatSamples + beatSamples / 2, out.getNumSamples());
}

TEST_CASE ("live: a clip moved while its note sounds ends the note, and plays from its new place")
{
    for (const std::string track : { "", "trk-lead" })
    {
        CAPTURE (track);
        FakeAudioDevice device;
        commands::CommandBus bus (heldNoteProject());
        engine::EditSync sync (testEngine(), bus);
        auto& bassMidi = MidiRecorder::addTo (sync.edit(), 0);
        auto& leadMidi = MidiRecorder::addTo (sync.edit(), 1);
        REQUIRE (bus.execute (commands::Play {}, testing::lee).ok);

        // At 1.2, to 2.1, on its own track or the lead.
        device.play (beatSamples);
        run (bus, sync, commands::MoveClip { "clp-held", "2.1", track });
        device.play (2 * barSamples - beatSamples);
        REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
        device.play (FakeAudioDevice::blockSize);

        // The note ends at the move, and nothing is left on.
        const auto bassEvents = bassMidi.recorded();
        const auto ended = std::ranges::find_if (bassEvents, [] (const MidiRecorder::Event& e) { return e.sample > 0 && (e.note < 0 || ! e.on); });
        REQUIRE (ended != bassEvents.end());
        CHECK_MESSAGE (ended->sample <= beatSamples + FakeAudioDevice::blockSize, historyOf (48, bassEvents));
        CHECK (notesLeftOn (bassEvents).empty());
        CHECK (notesLeftOn (leadMidi.recorded()).empty());

        const auto& out = device.output().buffer;
        checkSilent (out, beatSamples + beatSamples / 2, barSamples - FakeAudioDevice::blockSize);
        CHECK (nearSample (firstSoundAt (out, barSamples - beatSamples / 2), barSamples));
    }
}

TEST_CASE ("live: changes to other tracks don't stop or retrigger a held note")
{
    struct Change
    {
        const char* what;
        std::function<commands::Command()> command;
    };

    const Change changes[] = {
        { "a track added, with a synth", [] { return commands::AddTrack { "midi", "Keys", {}, {}, commands::Instrument { "hocket:synth", {} } }; } },
        { "a track removed", [] { return commands::RemoveTrack { "trk-lead" }; } },
        { "a track moved", [] { return commands::MoveTrack { "trk-lead", { "trk-bass", {} } }; } },
        { "a track renamed", [] { return commands::RenameTrack { "trk-lead", "Melody" }; } },
        { "an instrument removed", [] { return commands::SetInstrument { "trk-lead", {} }; } },
        { "a clip added", [] { return commands::AddClip { "trk-lead", "2.1", commands::ClipText { shortNote }, {}, {} }; } },
        { "a clip moved", [] { return commands::MoveClip { "clp-later", "4.1", {} }; } },
        { "a clip moved between tracks", [] { return commands::MoveClip { "clp-later", "2.1", "trk-bass" }; } },
        { "a clip removed", [] { return commands::RemoveClip { "clp-later" }; } },
    };

    for (const auto& change : changes)
    {
        CAPTURE (change.what);
        FakeAudioDevice device;
        commands::CommandBus bus (heldNoteProject());
        engine::EditSync sync (testEngine(), bus);
        REQUIRE (bus.execute (commands::Play {}, testing::lee).ok);

        // The change at 1.2, then on to just before 1.3. Nothing else sounds in bar 1.
        device.play (beatSamples);
        run (bus, sync, change.command());
        device.play (beatSamples - FakeAudioDevice::blockSize);

        // Loudness in 20 ms windows from just before the change, against the
        // steady note before it: 4OSC's phases differ from run to run, so
        // loudness can be compared but samples can't (as in LivePlaybackTests).
        const auto& out = device.output().buffer;
        constexpr int window = 882;
        const auto steady = out.getRMSLevel (0, beatSamples / 2, beatSamples / 2);
        auto quietest = 1.0e9f;
        auto loudest = 0.0f;

        for (int start = beatSamples - window; start + window <= out.getNumSamples(); start += window / 2)
        {
            const auto level = out.getRMSLevel (0, start, window) / steady;
            quietest = std::min (quietest, level);
            loudest = std::max (loudest, level);
        }

        CHECK_MESSAGE (quietest > 0.9f, "quietest " << quietest);
        CHECK_MESSAGE (loudest < 1.1f, "loudest " << loudest);
    }
}

TEST_CASE ("live: when the song ends before the playhead, it wraps as if the loop had always been that long, keeping its place in the bar")
{
    // A short note at 1.1 on the bass; the lead's clip at 3.1 makes the song three bars.
    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({
        makeTrack ("trk-bass", "Bass", { makeClip ("clp-a", 0, shortNote) }),
        makeTrack ("trk-lead", "Lead", { makeClip ("clp-later", 2 * bar, shortNote) }),
    }));
    engine::EditSync sync (testEngine(), bus);
    auto& midi = MidiRecorder::addTo (sync.edit());
    REQUIRE (bus.execute (commands::Play {}, testing::lee).ok);

    // At 2.2, the song shrinks to one bar. Wrapped, the playhead is at 1.2,
    // so the next note, at 1.1, comes three beats later, where bar 3 would
    // have started. Jumping to 1.1 would sound at once; playing on to the old
    // end would sound a bar later.
    const auto changeAt = barSamples + beatSamples;
    device.play (changeAt);
    run (bus, sync, commands::RemoveClip { "clp-later" });
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (2.0));
    device.play (2 * barSamples + beatSamples);

    REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
    device.play (FakeAudioDevice::blockSize);

    CHECK (notesLeftOn (midi.recorded()).empty());

    const auto& out = device.output().buffer;
    checkSilent (out, changeAt, 2 * barSamples - FakeAudioDevice::blockSize);
    CHECK (nearSample (firstSoundAt (out, changeAt), 2 * barSamples));

    // And it loops a bar at a time from there.
    CHECK (nearSample (firstSoundAt (out, 2 * barSamples + beatSamples), 3 * barSamples));
}
