// Each track's volume, pan, mute and solo, as heard (Milestone 1's step 6):
// offline renders, and changes during playback through the fake device. A
// SteadyTone stands in for the synth's sound, so each channel's gain can be
// measured exactly; StructureSyncTests.cpp checks the gains each track's gain
// stage is given after every command.

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
    // project's mix and the Edit's master volume (0 dB, which the test of a
    // render at 0dB and C holds it to).
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

TEST_CASE ("render: at 0dB and C, a track reaches the render exactly as its instrument made it")
{
    // Nothing between the instrument and the render may change its level:
    // not the track's gain stage, nor Tracktion's volume plugin after it,
    // which EditSync leaves at unity, nor the Edit's master, which Tracktion
    // starts at -3 dB and EditSync sets to 0 dB. So this doesn't read
    // Tracktion's gains, as expectedPeaks() does: the answer is the tone's own
    // level, in every period, on both channels. The render is 24-bit, which
    // holds 0.25 exactly, so a gain off by as little as 0.00001 dB shows.
    commands::CommandBus bus (toneProject());
    const auto& track = *project::findTrack (bus.project(), "trk-tone");
    REQUIRE (track.volume == 0.0);
    REQUIRE (track.pan == 0);
    engine::EditSync sync (testEngine(), bus);
    addTone (sync);

    const auto audio = renderLoop (sync);
    REQUIRE (audio.buffer.getNumChannels() == 2);
    const auto from = audio.buffer.getNumSamples() / 4;

    for (int channel = 0; channel < 2; ++channel)
    {
        CAPTURE (channel);
        const auto peaks = periodPeaks (audio.buffer, channel, from);
        REQUIRE (peaks.size() > 100);
        int off = 0;
        float example = SteadyTone::level;

        for (const auto peak : peaks)
            if (peak != SteadyTone::level)
            {
                ++off;
                example = peak;
            }

        CHECK_MESSAGE (off == 0, off << " of " << peaks.size() << " periods peaked off the tone's level of " << SteadyTone::level
                                     << ", e.g. at " << example << " (" << juce::Decibels::gainToDecibels (example / SteadyTone::level) << " dB)");
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

        // The gain stage ramps a change over 15 ms; measure after 25.
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

//==============================================================================
// Volume and pan reach the audio thread as one pair of gains, the gain
// stage's, so a change ramps in a straight line from the old setting to the
// new. Before the gain stage, Tracktion's volume plugin took them as two
// parameters, a fader and a pan, and a block that started between the two
// headed for a gain neither set: -6.5dB R100 to +6dB C sent the right channel
// toward +12 dB, and a pan alone from R100 to C at 0dB sent it to +6 dB
// (PROGRESS.md, step 7, "Before 4b").
namespace
{
    // Each channel's gain at each sample of the tone, from `from` to the end:
    // the sample over the tone's value there, or NaN where the sine is too
    // near zero to tell. The tone's phase is found in the `settled` samples
    // before `from`, where the gain is steady.
    std::array<std::vector<double>, 2> toneGains (const juce::AudioBuffer<float>& out, int from, int settled)
    {
        const auto sine = [] (int i) { return std::sin (juce::MathConstants<double>::twoPi * (i % SteadyTone::period) / SteadyTone::period); };
        int offset = 0;
        double best = -1.0;

        for (int candidate = 0; candidate < SteadyTone::period; ++candidate)
        {
            double sum = 0.0;

            for (int i = from - settled; i < from; ++i)
                sum += (out.getSample (0, i) + out.getSample (1, i)) * sine (i + candidate);

            if (sum > best)
                best = sum, offset = candidate;
        }

        std::array<std::vector<double>, 2> gains;

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = from; i < out.getNumSamples(); ++i)
            {
                const auto value = SteadyTone::level * sine (i + offset);
                gains[static_cast<std::size_t> (channel)].push_back (std::abs (value) < 0.5 * SteadyTone::level ? std::numeric_limits<double>::quiet_NaN()
                                                                                                                 : out.getSample (channel, i) / value);
            }
        }

        return gains;
    }

    // Settles the tone at one setting, changes it to another, and checks each
    // channel's gain at every sample from the change: on the straight line
    // from the old gain to the new over the ramp, never outside the two, and
    // then at the new one.
    void checkStraightRamp (const char* fromVolume, const char* fromPan, const char* toVolume, const char* toPan)
    {
        CAPTURE (fromVolume);
        CAPTURE (fromPan);
        CAPTURE (toVolume);
        CAPTURE (toPan);

        FakeAudioDevice device;
        commands::CommandBus bus (toneProject());
        engine::EditSync sync (testEngine(), bus);
        addTone (sync);
        run (bus, commands::SetMix { .trackId = "trk-tone", .volume = fromVolume, .pan = fromPan });
        REQUIRE (timedPlay (bus, testing::lee).ok);
        device.play (100 * ms);

        const auto before = expectedPeaks (sync, bus.project());
        const auto changedAt = device.output().buffer.getNumSamples();
        run (bus, commands::SetMix { .trackId = "trk-tone", .volume = toVolume, .pan = toPan });
        device.play (12 * FakeAudioDevice::blockSize);
        const auto after = expectedPeaks (sync, bus.project());

        // The gain stage takes the new pair at the next block, which starts at changedAt.
        const auto gains = toneGains (device.output().buffer, changedAt, 4 * SteadyTone::period);
        const auto ramp = engine::GainStage::rampSamples (FakeAudioDevice::sampleRate);
        REQUIRE (gains[0].size() > static_cast<std::size_t> (ramp + 4 * SteadyTone::period));

        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            CAPTURE (channel);
            const auto from = (channel == 0 ? before.left : before.right) / SteadyTone::level;
            const auto to = (channel == 0 ? after.left : after.right) / SteadyTone::level;
            const auto low = std::min (from, to);
            const auto high = std::max (from, to);
            int offLine = 0;
            int outside = 0;
            std::size_t firstOff = 0;

            for (std::size_t i = 0; i < gains[channel].size(); ++i)
            {
                const auto gain = gains[channel][i];

                if (std::isnan (gain))
                    continue;

                const auto k = std::min<double> (static_cast<double> (i + 1), ramp);
                const auto line = from + (to - from) * k / ramp;
                const auto tolerance = 1.0e-5 * (1.0 + high);

                if (std::abs (gain - line) > tolerance && offLine++ == 0)
                    firstOff = i;

                if (gain < low - tolerance || gain > high + tolerance)
                    ++outside;
            }

            CHECK_MESSAGE (offLine == 0, offLine << " samples off the straight line from " << from << " to " << to << ", the first at sample " << firstOff
                                                  << " of the ramp: " << gains[channel][firstOff]);
            CHECK_MESSAGE (outside == 0, outside << " samples outside " << low << " to " << high);
        }
    }
}

TEST_CASE ("live: volume and pan changed together, or pan alone, ramp straight to the new setting and never past it")
{
    // Together: the right channel from 0.47 (-6.5 dB) to 2.0 (+6 dB), where
    // Tracktion's two parameters once sent it toward 4.0.
    checkStraightRamp ("-6.5dB", "R100", "+6dB", "C");
    checkStraightRamp ("-6.5dB", "L50", "+6dB", "R1");

    // Pan alone: the near channel stays exactly where it is, both ways.
    checkStraightRamp ("0dB", "R100", "0dB", "C");
    checkStraightRamp ("0dB", "C", "0dB", "R100");

    // Volume alone, and the far side moving through the centre.
    checkStraightRamp ("-20dB", "L30", "+3dB", "L30");
    checkStraightRamp ("0dB", "L100", "-6dB", "R100");
}
