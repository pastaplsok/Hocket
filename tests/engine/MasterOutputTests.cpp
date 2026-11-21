// --play's master level and output meter (SPEC §9): a gain on everything
// Tracktion sends to the device, after the Edit, and what the meter says.

#include "EngineTesting.h"

#include "engine/MasterOutput.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // A quarter of a second at the fake device's rate.
    constexpr int window = 11025;

    // The loudest sample of the last window played.
    float lastWindowPeak (const FakeAudioDevice& device)
    {
        const auto& buffer = device.output().buffer;
        return peakBetween (buffer, buffer.getNumSamples() - window, buffer.getNumSamples());
    }

    double decibels (float ratio)
    {
        return static_cast<double> (juce::Decibels::gainToDecibels (ratio, -1000.0f));
    }

    constexpr double minusInfinity = -std::numeric_limits<double>::infinity();
}

TEST_CASE ("the master level scales what the device plays, and its meter reads what reached the device")
{
    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 1bar\n\n1.1 C3 1/1 v100\n") }) }));
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // Past its attack, the held note's level is steady. 4OSC's phases differ
    // from run to run, so levels are compared within this one.
    device.play (window);
    device.play (window);
    const auto unscaled = lastWindowPeak (device);
    REQUIRE (unscaled > 0.05f);

    {
        const engine::MasterOutput master (testEngine().getDeviceManager(), -12.0);
        device.play (window);
        const auto scaled = lastWindowPeak (device);

        CHECK_MESSAGE (std::abs (decibels (scaled / unscaled) + 12.0) < 0.3, "the level changed by " << decibels (scaled / unscaled) << " dB");
        CHECK (master.peak() == scaled); // the very samples the device got
        CHECK (master.clippedSamples() == 0);
    }

    {
        const engine::MasterOutput master (testEngine().getDeviceManager(), minusInfinity);
        device.play (window);

        CHECK (lastWindowPeak (device) == 0.0f);
        CHECK (master.peak() == 0.0f);
    }

    // Taken out, the note is as loud as before.
    device.play (window);
    const auto after = decibels (lastWindowPeak (device) / unscaled);
    CHECK_MESSAGE (std::abs (after) < 0.3, "the level changed by " << after << " dB");
}

TEST_CASE ("the meter counts the samples over full scale, and the warning suggests a level that keeps under it")
{
    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-keys", "Keys", { makeClip ("clp-keys", 0, "@length 1bar\n\n1.1 [C3 E3 G3 C4 E4] 1/1 v127\n") }) }));
    engine::EditSync sync (testEngine(), bus);
    const engine::MasterOutput master (testEngine().getDeviceManager(), 6.0);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    device.play (2 * window);

    const auto& buffer = device.output().buffer;
    std::uint64_t over = 0;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            over += std::abs (buffer.getSample (channel, i)) > 1.0f ? 1 : 0;

    REQUIRE_MESSAGE (master.peak() > 1.0f, "the chord peaked at " << decibels (master.peak()) << " dBFS: make it louder");
    CHECK (master.peak() == peakBetween (buffer, 0, buffer.getNumSamples()));
    CHECK (master.clippedSamples() == over);

    // The suggestion, played, keeps the peak under full scale.
    const auto warning = engine::clippingWarning (master.peak(), master.clippedSamples(), master.volume());
    INFO (warning);
    const auto at = warning.find ("--master-volume ");
    REQUIRE (at != std::string::npos);
    const auto suggested = std::stod (warning.substr (at + 16));
    CHECK (decibels (master.peak()) - master.volume() + suggested <= -1.0);
}

TEST_CASE ("the peak is described in dBFS, and clipping is warned about with a level to try")
{
    const auto gain = [] (double db) { return juce::Decibels::decibelsToGain (static_cast<float> (db)); };

    CHECK (engine::describePeak (0.0f) == "the output was silent");
    CHECK (engine::describePeak (gain (-23.1)) == "peak output level -23.1 dBFS");
    CHECK (engine::describePeak (1.0f) == "peak output level 0.0 dBFS");
    CHECK (engine::describePeak (gain (-0.01)) == "peak output level 0.0 dBFS"); // not "-0.0"
    CHECK (engine::describePeak (gain (2.3)) == "peak output level +2.3 dBFS");

    // Nothing over full scale, nothing to say.
    CHECK (engine::clippingWarning (0.9f, 0, -12.0).empty());
    CHECK (engine::clippingWarning (1.0f, 0, -12.0).empty());
    CHECK (engine::clippingWarning (gain (3.0), 10, minusInfinity).empty());

    // -12 - 2.3 - 1 is -15.3, so -16dB: whole dB, at least 1 dB under full scale.
    CHECK (engine::clippingWarning (gain (2.3), 1234, -12.0)
           == "Warning: the output clipped: 1234 samples went over full scale (the peak was +2.3 dBFS), and the driver cuts those off, "
              "which distorts the sound. --master-volume -16dB would have kept the peak under full scale.");

    CHECK (engine::clippingWarning (gain (0.5), 1, 0.0)
           == "Warning: the output clipped: 1 sample went over full scale (the peak was +0.5 dBFS), and the driver cuts those off, "
              "which distorts the sound. --master-volume -2dB would have kept the peak under full scale.");

    // Never below what --master-volume takes.
    CHECK (engine::clippingWarning (gain (120.0), 5, 6.0).find ("--master-volume -96dB ") != std::string::npos);
}

TEST_CASE ("the bus's output level restarts at each transport play, while the run's total carries on")
{
    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-keys", "Keys", { makeClip ("clp-keys", 0, "@length 1bar\n\n1.1 [C3 E3 G3 C4 E4] 1/1 v127\n") }) }));
    engine::EditSync sync (testEngine(), bus);
    engine::MasterOutput master (testEngine().getDeviceManager(), 6.0);
    bus.setOutputMeter (&master);

    REQUIRE (timedPlay (bus, testing::lee).ok);
    device.play (2 * window);
    REQUIRE (master.clippedSamples() > 0);
    CHECK (master.levelSincePlay().peak == master.peak());
    CHECK (master.levelSincePlay().clippedSamples == master.clippedSamples());

    // Stopped, and played again: the level starts from nothing, the run's total doesn't.
    REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
    device.play (window);
    const auto runPeak = master.peak();
    const auto runClipped = master.clippedSamples();

    REQUIRE (timedPlay (bus, testing::lee).ok);
    CHECK (master.levelSincePlay().peak == 0.0f);
    CHECK (master.levelSincePlay().clippedSamples == 0);
    CHECK (master.peak() == runPeak);
    CHECK (master.clippedSamples() == runClipped);

    device.play (2 * window);
    CHECK (master.levelSincePlay().peak > 1.0f);
    CHECK (master.levelSincePlay().clippedSamples > 0);
    CHECK (master.clippedSamples() == runClipped + master.levelSincePlay().clippedSamples);

    bus.setOutputMeter (nullptr);
}
