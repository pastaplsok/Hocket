// Live playback through the fake audio device: what Milestone 0 is about.
// A write_clip during playback must land by the next pass, with no glitch.

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

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

    project::Project oneClip (std::string_view notesText)
    {
        return makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, notesText) }) });
    }

    // What the app does on the message thread straight after a command: Tracktion
    // rebuilds its playback graph on a 1 ms timer, and the tests can't wait for it.
    void rebuildGraph (engine::EditSync& sync)
    {
        sync.edit().dispatchPendingUpdatesSynchronously();
    }

    struct Loudness
    {
        float quietest = 1.0e9f;
        float loudest = 0.0f;
    };

    // Plays a whole-bar C3, rewrites the clip at 1.2, and plays on to just
    // before 1.3. Returns the quietest and loudest 20 ms window from just before
    // the rewrite onwards, relative to the steady note before it. 4OSC's
    // oscillator phases differ from run to run, so loudness can be compared but
    // samples can't.
    Loudness heldNoteAcrossRewrite (const std::string& rewrite)
    {
        FakeAudioDevice device;
        commands::CommandBus bus (oneClip ("@length 1bar\n\n1.1 C3 1/1 v100\n"));
        engine::EditSync sync (testEngine(), bus);
        REQUIRE (bus.execute (commands::Play {}, "user").ok);

        device.play (beatSamples);
        REQUIRE (bus.execute (commands::WriteClip { "clp-bass", rewrite }, "Claude").ok);
        rebuildGraph (sync);
        device.play (beatSamples - FakeAudioDevice::blockSize);

        const auto& out = device.output().buffer;
        constexpr int window = 882;
        const auto steady = out.getRMSLevel (0, beatSamples / 2, beatSamples / 2); // after the attack
        Loudness loudness;

        for (int start = beatSamples - window; start + window <= out.getNumSamples(); start += window / 2)
        {
            const auto level = out.getRMSLevel (0, start, window) / steady;
            loudness.quietest = std::min (loudness.quietest, level);
            loudness.loudest = std::max (loudness.loudest, level);
        }

        return loudness;
    }
}

TEST_CASE ("live: play loops the song")
{
    FakeAudioDevice device;
    commands::CommandBus bus (oneClip ("@length 1bar\n\n1.1 C3 1/8 v100\n"));
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (bus.execute (commands::Play {}, "user").ok);

    device.play (2 * barSamples + beatSamples);
    const auto& out = device.output().buffer;

    CHECK (nearSample (firstSoundAt (out), 0));
    CHECK (nearSample (firstSoundAt (out, barSamples - beatSamples), barSamples));
    CHECK (nearSample (firstSoundAt (out, 2 * barSamples - beatSamples), 2 * barSamples));
}

TEST_CASE ("live: a write_clip mid-loop is heard by the next pass, and what played before it is untouched")
{
    FakeAudioDevice device;
    commands::CommandBus bus (oneClip ("@length 1bar\n\n1.1 C3 1/8 v100\n"));
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (bus.execute (commands::Play {}, "user").ok);

    // Play to 1.2.3, then move the note from 1.1 to 1.3.
    device.play (beatSamples + beatSamples / 2);
    REQUIRE (bus.execute (commands::WriteClip { "clp-bass", "@length 1bar\n\n1.3 C3 1/8 v100\n" }, "Claude").ok);
    rebuildGraph (sync);
    device.play (barSamples - beatSamples - beatSamples / 2 + barSamples);

    const auto& out = device.output().buffer;

    // First pass: the old note at 1.1, then the new note at 1.3, since the edit came before it.
    CHECK (nearSample (firstSoundAt (out), 0));
    CHECK (nearSample (firstSoundAt (out, beatSamples + beatSamples / 2), 2 * beatSamples));

    // Second pass: only the new note.
    CHECK (peakBetween (out, barSamples, barSamples + 2 * beatSamples - 441) < 1.0e-3f);
    CHECK (nearSample (firstSoundAt (out, barSamples), barSamples + 2 * beatSamples));
}

TEST_CASE ("live: a rewrite that keeps a held note doesn't retrigger it or cut it off")
{
    // Adds a note at 1.3 and keeps the held one.
    const auto loudness = heldNoteAcrossRewrite ("@length 1bar\n\n1.1 C3 1/1 v100\n1.3 G3 1/4 v100\n");
    CHECK_MESSAGE (loudness.quietest > 0.9f, "quietest " << loudness.quietest);
    CHECK_MESSAGE (loudness.loudest < 1.1f, "loudest " << loudness.loudest);
}

TEST_CASE ("live: the held-note check can fail: restriking the note shows up")
{
    // Ends the note at 1.2 and strikes it again there, so its attack restarts.
    const auto loudness = heldNoteAcrossRewrite ("@length 1bar\n\n1.1 C3 1/4 v100\n1.2 C3 3/4 v100\n");
    CHECK_MESSAGE ((loudness.quietest < 0.9f || loudness.loudest > 1.1f), "quietest " << loudness.quietest << ", loudest " << loudness.loudest);
}

TEST_CASE ("live: stop silences playback")
{
    FakeAudioDevice device;
    commands::CommandBus bus (oneClip ("@length 1bar\n\n1.1 C3 1/1 v100\n"));
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (bus.execute (commands::Play {}, "user").ok);

    device.play (beatSamples);
    REQUIRE (bus.execute (commands::Stop {}, "user").message == "stopped");
    device.play (beatSamples);

    const auto& out = device.output().buffer;
    CHECK (peakBetween (out, 0, beatSamples) > 0.05f);
    CHECK (peakBetween (out, beatSamples + beatSamples / 2, 2 * beatSamples) < 1.0e-3f);
}
