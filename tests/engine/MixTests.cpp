// Each track's volume, pan, mute and solo, as heard (Milestone 1's step 6):
// offline renders, and changes during playback through the fake device. A
// SteadyTone stands in for the synth's sound, so each channel's gain can be
// measured exactly; StructureSyncTests.cpp checks the gains Tracktion is
// given after every command.

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // trk-tone plays a SteadyTone once addTone() has put one on it; trk-quiet
    // makes no sound, and is there to be soloed. One bar: 2 s at 120 BPM.
    project::Project toneProject()
    {
        return makeProject ({ makeTrack ("trk-tone", "Tone", { makeClip ("clp-tone", 0, "@length 1bar\n") }),
                              makeTrack ("trk-quiet", "Quiet", { makeClip ("clp-quiet", 0, "@length 1bar\n") }) });
    }

    void addTone (engine::EditSync& sync)
    {
        SteadyTone::addTo (*tracktion::getAudioTracks (sync.edit())[0]);
    }

    // The loudest sample the tone should reach on each channel, given the
    // project's mix and the Edit's master volume (Tracktion's default, -3 dB).
    ChannelGains expectedPeaks (engine::EditSync& sync, const project::Project& project)
    {
        const auto track = specifiedGains (project, *project::findTrack (project, "trk-tone"));
        const auto master = volumePluginGains (*sync.edit().getMasterVolumePlugin());
        return { SteadyTone::level * track.left * master.left, SteadyTone::level * track.right * master.right };
    }

    // Within 0.01%, or silent.
    bool closeTo (float actual, double expected)
    {
        return std::abs (static_cast<double> (actual) - expected) <= 1.0e-4 * expected + 1.0e-7;
    }

    // The loudest sample on a channel in each run of SteadyTone::period
    // samples, from `start` to the end.
    std::vector<float> periodPeaks (const juce::AudioBuffer<float>& buffer, int channel, int start)
    {
        std::vector<float> peaks;

        for (int i = start; i + SteadyTone::period <= buffer.getNumSamples(); i += SteadyTone::period)
            peaks.push_back (buffer.getMagnitude (channel, i, SteadyTone::period));

        return peaks;
    }

    void run (commands::CommandBus& bus, const commands::Command& command)
    {
        const auto result = bus.execute (command, testing::claude);
        INFO (result.message);
        REQUIRE (result.ok);
    }

    constexpr int ms = 44; // samples, near enough, at the fake device's 44.1 kHz
}

TEST_CASE ("render: each channel's level follows the track's volume and pan, by the balance law")
{
    struct Case
    {
        std::string volume;
        std::string pan;
    };

    // At C both channels get the volume; panned, the near one does, and the far one less, to none at 100.
    for (const auto& c : { Case { "0dB", "C" }, Case { "0dB", "L50" }, Case { "-6dB", "R100" }, Case { "+3dB", "R20" } })
    {
        CAPTURE (c.volume);
        CAPTURE (c.pan);
        commands::CommandBus bus (toneProject());
        engine::EditSync sync (testEngine(), bus);
        addTone (sync);
        run (bus, commands::SetMix { .trackId = "trk-tone", .volume = c.volume, .pan = c.pan });

        const auto audio = renderLoop (sync);
        const auto expected = expectedPeaks (sync, bus.project());
        const auto middle = audio.buffer.getNumSamples() / 4;
        const auto left = audio.buffer.getMagnitude (0, middle, middle * 2);
        const auto right = audio.buffer.getMagnitude (1, middle, middle * 2);

        CHECK_MESSAGE (closeTo (left, expected.left), "left " << left << ", should be " << expected.left);
        CHECK_MESSAGE (closeTo (right, expected.right), "right " << right << ", should be " << expected.right);
    }
}

TEST_CASE ("live: each channel's level follows volume, pan, mute and solo during playback")
{
    FakeAudioDevice device;
    commands::CommandBus bus (toneProject());
    engine::EditSync sync (testEngine(), bus);
    addTone (sync);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    using commands::SetMix;
    const std::vector<SetMix> steps {
        { .trackId = "trk-tone", .volume = "-6dB" },
        { .trackId = "trk-tone", .pan = "L50" },
        { .trackId = "trk-tone", .volume = "+6dB", .pan = "R100" },
        { .trackId = "trk-tone", .volume = "-20dB", .pan = "L1" },
        { .trackId = "trk-tone", .volume = "-infdB", .pan = "C" },
        { .trackId = "trk-tone", .volume = "0dB" },
        { .trackId = "trk-tone", .mute = true },
        { .trackId = "trk-tone", .mute = false },
        { .trackId = "trk-quiet", .solo = true }, // silent: another track is soloed
        { .trackId = "trk-tone", .solo = true },  // heard again
        { .trackId = "trk-tone", .mute = true },  // muted, though soloed
        { .trackId = "trk-tone", .mute = false, .solo = false },
        { .trackId = "trk-quiet", .solo = false },
    };

    for (const auto& step : steps)
    {
        CAPTURE (step.trackId);
        CAPTURE (step.volume.value_or (""));
        CAPTURE (step.pan.value_or (""));
        CAPTURE (step.mute.has_value() ? (*step.mute ? "mute" : "unmute") : "");
        CAPTURE (step.solo.has_value() ? (*step.solo ? "solo" : "unsolo") : "");
        run (bus, step);

        // The plugin smooths a change over 15 ms; measure after 25.
        device.play (50 * ms);
        const auto& out = device.output().buffer;
        const auto from = out.getNumSamples() - 25 * ms;
        const auto expected = expectedPeaks (sync, bus.project());
        const auto left = out.getMagnitude (0, from, 25 * ms);
        const auto right = out.getMagnitude (1, from, 25 * ms);

        CHECK_MESSAGE (closeTo (left, expected.left), "left " << left << ", should be " << expected.left);
        CHECK_MESSAGE (closeTo (right, expected.right), "right " << right << ", should be " << expected.right);
    }
}

TEST_CASE ("live: a pan change during a held note leaves the near channel flat while the far one moves")
{
    // The tone is the held note: its level never drifts, so "flat" can be exact.
    FakeAudioDevice device;
    commands::CommandBus bus (toneProject());
    engine::EditSync sync (testEngine(), bus);
    addTone (sync);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    device.play (500 * ms);
    const auto centred = expectedPeaks (sync, bus.project());
    const auto changedAt = device.output().buffer.getNumSamples();
    run (bus, commands::SetMix { .trackId = "trk-tone", .pan = "L50" });
    device.play (250 * ms);
    const auto panned = expectedPeaks (sync, bus.project());
    REQUIRE (panned.left == doctest::Approx (centred.left));
    REQUIRE (panned.right == doctest::Approx (centred.right / 2.0));

    const auto& out = device.output().buffer;
    const auto from = changedAt - 100 * ms;
    const auto near = periodPeaks (out, 0, from);
    const auto far = periodPeaks (out, 1, from);

    // Left, the near side, is the same in every period, before, during and after.
    int nearMoved = 0;

    for (const auto peak : near)
        nearMoved += closeTo (peak, centred.left) ? 0 : 1;

    CHECK_MESSAGE (nearMoved == 0, nearMoved << " of " << near.size() << " periods on the near side weren't at its level");

    // Right goes from the centred level to half, never back up, and over
    // about 15 ms rather than at once.
    int falling = 0;

    for (std::size_t i = 0; i < far.size(); ++i)
    {
        CAPTURE (i);

        if (i > 0)
            CHECK (far[i] <= far[i - 1] + 1.0e-6f);

        if (! closeTo (far[i], centred.right) && ! closeTo (far[i], panned.right))
            ++falling;
    }

    CHECK (closeTo (far.front(), centred.right));
    CHECK (closeTo (far.back(), panned.right));
    CHECK_MESSAGE ((falling >= 10 * ms / SteadyTone::period && falling <= 20 * ms / SteadyTone::period),
                   "the far side took " << falling * SteadyTone::period << " samples to move");
}

TEST_CASE ("live: mute fades out over about 15 ms and back in, rather than cutting")
{
    FakeAudioDevice device;
    commands::CommandBus bus (toneProject());
    engine::EditSync sync (testEngine(), bus);
    addTone (sync);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    device.play (200 * ms);
    const auto heard = expectedPeaks (sync, bus.project()).left;
    const auto mutedAt = device.output().buffer.getNumSamples();
    run (bus, commands::SetMix { .trackId = "trk-tone", .mute = true });
    device.play (100 * ms);
    const auto unmutedAt = device.output().buffer.getNumSamples();
    run (bus, commands::SetMix { .trackId = "trk-tone", .mute = false });
    device.play (100 * ms);

    const auto& out = device.output().buffer;

    for (int channel = 0; channel < 2; ++channel)
    {
        CAPTURE (channel);
        const auto fadingOut = periodPeaks (out, channel, mutedAt);
        const auto fadingIn = periodPeaks (out, channel, unmutedAt);
        const auto periods = 20 * ms / SteadyTone::period; // the fade is over by then

        // A linear fade over 661 samples: the tone peaks every 50 samples, so
        // the first period's loudest is within 50 to 100 samples of the start.
        // Out: still nearly full in its first period, down every period after, then silent.
        CHECK (fadingOut.front() > 0.9f * static_cast<float> (heard));

        for (int i = 1; i < periods; ++i)
            CHECK (fadingOut[static_cast<std::size_t> (i)] <= fadingOut[static_cast<std::size_t> (i - 1)]);

        const auto silentFrom = mutedAt + periods * SteadyTone::period;
        CHECK (out.getMagnitude (channel, silentFrom, unmutedAt - silentFrom) == 0.0f);

        // In: from nearly nothing, up every period, back to the level.
        CHECK (fadingIn.front() < 0.2f * static_cast<float> (heard));

        for (int i = 1; i < periods; ++i)
            CHECK (fadingIn[static_cast<std::size_t> (i)] >= fadingIn[static_cast<std::size_t> (i - 1)]);

        CHECK (closeTo (fadingIn.back(), heard));
    }
}

TEST_CASE ("live: a volume set during playback stays set when the loop comes round, and after play again")
{
    // Tracktion can record a parameter's changes during playback as
    // automation, and play that back, overriding the mix. It flushes what it
    // records on a timer, and commits it when the transport stops, so the
    // message loop runs here between steps, as it does in the app.
    const auto handleMessages = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (150); };

    FakeAudioDevice device;
    commands::CommandBus bus (toneProject());
    engine::EditSync sync (testEngine(), bus);
    addTone (sync);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    device.play (500 * ms);
    run (bus, commands::SetMix { .trackId = "trk-tone", .volume = "-6dB" });
    handleMessages();
    device.play (500 * ms);
    run (bus, commands::SetMix { .trackId = "trk-tone", .volume = "-12dB" });
    handleMessages();
    const auto expected = expectedPeaks (sync, bus.project()).left;

    // On to 0.5 s into the next pass: the loop is 2 s.
    const auto setAt = device.output().buffer.getNumSamples();
    device.play (1500 * ms);
    handleMessages();
    const auto& out = device.output().buffer;
    const auto settled = setAt + 25 * ms;
    CHECK (closeTo (out.getMagnitude (0, settled, out.getNumSamples() - settled), expected));
    CHECK (closeTo (out.getMagnitude (0, out.getNumSamples() - 500 * ms, 500 * ms), expected)); // after the wrap: not quieter or louder

    REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
    handleMessages();
    REQUIRE (timedPlay (bus, testing::lee).ok);
    device.play (750 * ms);
    CHECK (closeTo (out.getMagnitude (0, out.getNumSamples() - 500 * ms, 500 * ms), expected));

    auto* volume = tracktion::getAudioTracks (sync.edit())[0]->getVolumePlugin();
    CHECK_FALSE (volume->volParam->hasAutomationPoints());
}
