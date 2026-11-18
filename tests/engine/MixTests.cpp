// Each track's volume, pan, mute and solo, as heard (Milestone 1's step 6):
// offline renders, and changes during playback through the fake device. A
// SteadyTone stands in for the synth's sound, so each channel's gain can be
// measured exactly; StructureSyncTests.cpp checks the gains Tracktion is
// given after every command.

#include "EngineTesting.h"
#include "core/Paths.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
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
    // not the track's volume plugin, nor the Edit's master, which Tracktion
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

//==============================================================================
// Step 6's volume-and-pan stress test failed about a third of the time, at a
// block boundary, as a ramp began toward a gain no change set (PROGRESS.md,
// Loose ends). EditSync::syncMix sets a track's fader position, then its pan:
// two parameters. A block that starts between the two gets the new fader with
// the old pan. Played here block by block, with what the volume plugin read
// for each block and each channel's gain at every sample.
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

    // The gain at sample i, or the nearest one after it that can be told.
    double gainNear (const std::vector<double>& gains, std::size_t i)
    {
        for (; i < gains.size(); ++i)
            if (! std::isnan (gains[i]))
                return gains[i];

        return std::numeric_limits<double>::quiet_NaN();
    }

    struct SplitRun
    {
        std::array<std::vector<double>, 2> gains; // from the first block after the fader was set
        std::vector<ChannelGains> targets;        // what the volume plugin aimed for, block by block
        ChannelGains before;                      // the old setting's gains
        ChannelGains after;                       // the new setting's
    };

    // Settles the tone at `from`, then sets `to`'s fader position, plays
    // `blocksBetween` blocks, sets its pan, and plays on: syncMix's two
    // parameters, with blocks between them. With none between, both arrive
    // together, as they almost always do.
    SplitRun playSplit (const char* fromVolume, const char* fromPan, const char* toVolume, const char* toPan, int blocksBetween)
    {
        FakeAudioDevice device;
        commands::CommandBus bus (toneProject());
        engine::EditSync sync (testEngine(), bus);
        addTone (sync);
        auto& volume = *tracktion::getAudioTracks (sync.edit())[0]->getVolumePlugin();

        run (bus, commands::SetMix { .trackId = "trk-tone", .volume = fromVolume, .pan = fromPan });
        REQUIRE (timedPlay (bus, testing::lee).ok);
        device.play (100 * ms);

        SplitRun result;
        result.before = volumePluginGains (volume);
        const auto oldFader = volume.volParam->getCurrentValue();
        const auto oldPan = volume.panParam->getCurrentValue();

        // The new setting's parameters, as syncMix makes them; then the old
        // ones back, before any block plays.
        run (bus, commands::SetMix { .trackId = "trk-tone", .volume = toVolume, .pan = toPan });
        result.after = volumePluginGains (volume);
        const auto newFader = volume.volParam->getCurrentValue();
        const auto newPan = volume.panParam->getCurrentValue();
        volume.volParam->setParameter (oldFader, juce::sendNotification);
        volume.panParam->setParameter (oldPan, juce::sendNotification);

        const auto changedAt = device.output().buffer.getNumSamples();
        constexpr int blocksAfter = 6;

        // The fader first, as syncMix sets it; the volume plugin reads both
        // parameters at the start of each block. How long setting one takes
        // is the window in which a block can start between the two.
        const auto settingStarted = std::chrono::steady_clock::now();
        volume.volParam->setParameter (newFader, juce::sendNotification);
        const auto settingTook = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - settingStarted).count();

        for (int block = 0; block < blocksBetween + blocksAfter; ++block)
        {
            if (block == blocksBetween)
                volume.panParam->setParameter (newPan, juce::sendNotification);

            result.targets.push_back (volumePluginGains (volume));
            device.play (FakeAudioDevice::blockSize);
        }

        result.gains = toneGains (device.output().buffer, changedAt, 4 * SteadyTone::period);

        // The log asked for: what the plugin read, and the gains through the ramp.
        std::ostringstream log;
        log << fromVolume << " " << fromPan << " to " << toVolume << " " << toPan << ", the pan " << blocksBetween
            << " block(s) after the fader (setting the fader took " << settingTook << " us):\n";

        for (std::size_t block = 0; block < result.targets.size(); ++block)
        {
            const auto first = block * static_cast<std::size_t> (FakeAudioDevice::blockSize);
            const auto last = first + FakeAudioDevice::blockSize - SteadyTone::period;
            log << "  block " << block << ": read fader " << juce::Decibels::gainToDecibels (tracktion::volumeFaderPositionToGain (newFader))
                << " dB, pan " << (block < static_cast<std::size_t> (blocksBetween) ? oldPan : newPan) << ": targets L " << result.targets[block].left
                << ", R " << result.targets[block].right << "; gains L " << gainNear (result.gains[0], first) << " to "
                << gainNear (result.gains[0], last) << ", R " << gainNear (result.gains[1], first) << " to " << gainNear (result.gains[1], last) << "\n";
        }

        MESSAGE (log.str());

        // The full trace, sample by sample, when asked for (a folder to write it in).
        if (const auto folder = juce::SystemStats::getEnvironmentVariable ("HOCKET_TEST_TRACE_DIR", {}); folder.isNotEmpty())
        {
            std::ofstream csv (pathFromUtf8 (folder.toStdString())
                               / pathFromUtf8 (std::string ("split-") + fromPan + "-" + toPan + "-" + std::to_string (blocksBetween) + ".csv"));
            csv << "sample,left,right\n";

            for (std::size_t i = 0; i < result.gains[0].size(); ++i)
                csv << i << "," << result.gains[0][i] << "," << result.gains[1][i] << "\n";
        }

        return result;
    }

    // The largest gain a channel reached, where it can be told.
    double highest (const std::vector<double>& gains)
    {
        double top = 0.0;

        for (const auto gain : gains)
            if (! std::isnan (gain))
                top = std::max (top, gain);

        return top;
    }
}

TEST_CASE ("live: a fader and pan that reach the audio thread in different blocks ramp toward a gain neither sets")
{
    const auto ramp = 0.015 * FakeAudioDevice::sampleRate; // samples: VolumeAndPanPlugin's linear smoothing

    // Together, as almost always: straight to the new setting, never past it.
    const auto together = playSplit ("-6.5dB", "R100", "+6dB", "C", 0);
    CHECK (together.targets.front().right == doctest::Approx (together.after.right));
    CHECK (highest (together.gains[1]) <= together.after.right * 1.0001);

    // One block between: for that block the plugin aims for the new fader
    // (+6 dB, Tracktion's 1.995) with the old pan (hard right, 1), so the
    // right channel heads for twice the new setting, +12 dB.
    const auto oneBlock = playSplit ("-6.5dB", "R100", "+6dB", "C", 1);
    const auto mixedTarget = 2.0 * oneBlock.after.right;
    CHECK (oneBlock.targets.front().right == doctest::Approx (mixedTarget));
    const auto endOfBlock = gainNear (oneBlock.gains[1], FakeAudioDevice::blockSize - SteadyTone::period);
    const auto towardMixed = oneBlock.before.right + (mixedTarget - oneBlock.before.right) * (FakeAudioDevice::blockSize - SteadyTone::period) / ramp;
    CHECK (endOfBlock == doctest::Approx (towardMixed).epsilon (0.01));

    // Three blocks between, as one block of 1024 samples would be: the ramp
    // gets there, 6 dB over the new setting, before coming back down.
    const auto threeBlocks = playSplit ("-6.5dB", "R100", "+6dB", "C", 3);
    CHECK (highest (threeBlocks.gains[1]) == doctest::Approx (mixedTarget).epsilon (0.001));
    CHECK (juce::Decibels::gainToDecibels (highest (threeBlocks.gains[1]) / threeBlocks.after.right) == doctest::Approx (6.02).epsilon (0.01));

    // The stress test's other failure: -6.5dB L50 to +6dB R1, on the left:
    // the new fader (+5.96 dB, as R1 takes its share) with the old pan (L50, -1/3).
    const auto other = playSplit ("-6.5dB", "L50", "+6dB", "R1", 1);
    CHECK (other.targets.front().left == doctest::Approx (2.647).epsilon (0.001));
    CHECK (other.after.left == doctest::Approx (1.975).epsilon (0.001));
}
