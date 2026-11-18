// The gain stage that plays each track's mix (Milestone 1's step 7, "Before
// 4b"), alone, with no Tracktion graph: both channels ramp together, exactly,
// sample by sample; the hand-over is one atomic value that's never read torn;
// and nothing is allocated on the audio thread. MixTests.cpp and the stress
// tests hear it on a track.

#include "AllocationCounter.h"
#include "engine/mix/GainStage.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using namespace hocket;
using engine::GainStage;
using engine::StereoGain;

namespace
{
    constexpr double rate = 44100.0;
    constexpr int ramp = 661; // 15 ms at 44.1 kHz, as juce::SmoothedValue counts it

    struct Stereo
    {
        std::vector<float> left;
        std::vector<float> right;
    };

    // Ones on both channels, so the output is the gain.
    Stereo ones (int length)
    {
        return { std::vector<float> (static_cast<std::size_t> (length), 1.0f), std::vector<float> (static_cast<std::size_t> (length), 1.0f) };
    }

    void process (GainStage& stage, Stereo& audio, int from, int count)
    {
        stage.process (audio.left.data() + from, audio.right.data() + from, count);
    }

    // What sample k of a ramp (1 to `ramp`) should be, worked out in double.
    double onRamp (double from, double to, int k)
    {
        return from + (to - from) * static_cast<double> (k) / ramp;
    }
}

TEST_CASE ("gain stage: a ramp is 15 ms, counted as Tracktion's smoothing counts it")
{
    CHECK (GainStage::rampSamples (44100.0) == ramp);
    CHECK (GainStage::rampSamples (48000.0) == 720);
    CHECK (GainStage::rampSamples (96000.0) == 1440);
    CHECK (GainStage::rampSamples (22050.0) == 330);
}

TEST_CASE ("gain stage: it starts at its target, with no ramp, and settled gains are exact")
{
    GainStage stage;
    CHECK (stage.target() == StereoGain { 1.0f, 1.0f });

    stage.setTarget ({ 0.3f, 0.7f });
    stage.prepare (rate);
    CHECK (stage.current() == StereoGain { 0.3f, 0.7f });

    // Any samples, each multiplied by exactly the target.
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> value (-1.0f, 1.0f);
    Stereo audio { std::vector<float> (1000), std::vector<float> (1000) };

    for (std::size_t i = 0; i < 1000; ++i)
    {
        audio.left[i] = value (rng);
        audio.right[i] = value (rng);
    }

    const auto input = audio;
    process (stage, audio, 0, 1000);

    for (std::size_t i = 0; i < 1000; ++i)
    {
        CAPTURE (i);
        REQUIRE (audio.left[i] == input.left[i] * 0.3f);
        REQUIRE (audio.right[i] == input.right[i] * 0.7f);
    }

    // At unity it changes nothing.
    stage.setTarget ({});
    stage.prepare (rate);
    auto untouched = input;
    process (stage, untouched, 0, 1000);
    CHECK (untouched.left == input.left);
    CHECK (untouched.right == input.right);
}

TEST_CASE ("gain stage: both channels ramp together in a straight line over 15 ms, and end exactly at the new pair")
{
    struct Case
    {
        StereoGain from;
        StereoGain to;
    };

    // Up and down, one channel still, one to silence, and from silence.
    for (const auto& c : { Case { { 1.0f, 1.0f }, { 0.5f, 0.25f } }, Case { { 0.473f, 0.473f }, { 1.995f, 1.995f } }, Case { { 1.0f, 0.0f }, { 1.0f, 1.0f } },
                           Case { { 0.0f, 0.0f }, { 2.0f, 0.1f } }, Case { { 0.8f, 0.3f }, { 0.0f, 0.0f } } })
    {
        CAPTURE (c.from.left);
        CAPTURE (c.from.right);
        CAPTURE (c.to.left);
        CAPTURE (c.to.right);
        GainStage stage;
        stage.setTarget (c.from);
        stage.prepare (rate);
        stage.setTarget (c.to);

        auto audio = ones (2000);
        process (stage, audio, 0, 2000);

        for (int i = 0; i < 2000; ++i)
        {
            CAPTURE (i);
            const auto at = static_cast<std::size_t> (i);

            if (i + 1 < ramp)
            {
                // Sample k of the ramp, the first included, is k / ramp of the way there.
                REQUIRE (audio.left[at] == doctest::Approx (onRamp (c.from.left, c.to.left, i + 1)).epsilon (1.0e-6));
                REQUIRE (audio.right[at] == doctest::Approx (onRamp (c.from.right, c.to.right, i + 1)).epsilon (1.0e-6));
            }
            else
            {
                // The last sample of the ramp, and every one after, exactly the new pair.
                REQUIRE (audio.left[at] == c.to.left);
                REQUIRE (audio.right[at] == c.to.right);
            }
        }

        CHECK (stage.current() == c.to);
    }
}

TEST_CASE ("gain stage: a pair arriving mid-ramp starts a new ramp from where the gains are")
{
    GainStage stage;
    stage.prepare (rate);
    stage.setTarget ({ 0.0f, 2.0f });

    auto audio = ones (3000);
    process (stage, audio, 0, 300);
    const auto reached = stage.current();
    CHECK (reached.left == audio.left[299]);
    CHECK (reached.right == audio.right[299]);
    CHECK (reached.left == doctest::Approx (onRamp (1.0, 0.0, 300)).epsilon (1.0e-6));
    CHECK (reached.right == doctest::Approx (onRamp (1.0, 2.0, 300)).epsilon (1.0e-6));

    // Back to unity: from where they are, not from where the last ramp
    // started or was going, over a whole ramp again.
    stage.setTarget ({ 1.0f, 1.0f });
    process (stage, audio, 300, 2700);

    for (int k = 1; k < ramp; ++k)
    {
        CAPTURE (k);
        const auto at = static_cast<std::size_t> (299 + k);
        REQUIRE (audio.left[at] == doctest::Approx (onRamp (reached.left, 1.0, k)).epsilon (1.0e-6));
        REQUIRE (audio.right[at] == doctest::Approx (onRamp (reached.right, 1.0, k)).epsilon (1.0e-6));
    }

    for (std::size_t i = 299 + ramp; i < 3000; ++i)
    {
        REQUIRE (audio.left[i] == 1.0f);
        REQUIRE (audio.right[i] == 1.0f);
    }
}

TEST_CASE ("gain stage: the same target again changes nothing, even mid-ramp")
{
    GainStage stage;
    stage.prepare (rate);
    stage.setTarget ({ 0.5f, 0.5f });
    auto once = ones (1000);
    process (stage, once, 0, 1000);

    GainStage again;
    again.prepare (rate);
    again.setTarget ({ 0.5f, 0.5f });
    auto twice = ones (1000);
    process (again, twice, 0, 200);
    again.setTarget ({ 0.5f, 0.5f });
    process (again, twice, 200, 800);

    CHECK (once.left == twice.left);
    CHECK (once.right == twice.right);
}

TEST_CASE ("gain stage: blocks of any size give the same output as one block, ramps and all")
{
    // Targets set at these samples, between blocks in both runs; the third
    // comes mid-ramp.
    struct Change
    {
        int at;
        StereoGain gain;
    };

    const std::vector<Change> changes { { 0, { 0.2f, 1.5f } }, { 1000, { 1.0f, 0.0f } }, { 1300, { 0.6f, 0.6f } }, { 3000, { 0.0f, 0.0f } }, { 3500, { 1.0f, 1.0f } } };
    constexpr int length = 5000;

    const auto play = [&changes] (auto nextBlock)
    {
        GainStage stage;
        stage.prepare (rate);
        auto audio = ones (length);
        int done = 0;

        for (std::size_t c = 0; c < changes.size(); ++c)
        {
            stage.setTarget (changes[c].gain);
            const auto end = c + 1 < changes.size() ? changes[c + 1].at : length;

            while (done < end)
            {
                const auto count = std::min (nextBlock(), end - done);
                process (stage, audio, done, count);
                done += count;
            }
        }

        return audio;
    };

    const auto whole = play ([] { return length; });

    for (const auto blockSize : { 1, 7, 64, 256, 441, 1024 })
    {
        CAPTURE (blockSize);
        const auto blocks = play ([blockSize] { return blockSize; });
        CHECK (blocks.left == whole.left);
        CHECK (blocks.right == whole.right);
    }

    std::mt19937 rng (42);
    std::uniform_int_distribution<int> size (1, 700);
    const auto mixed = play ([&rng, &size] { return size (rng); });
    CHECK (mixed.left == whole.left);
    CHECK (mixed.right == whole.right);
}

TEST_CASE ("gain stage: a mono block ramps its one channel, and the ramp moves on as in stereo")
{
    GainStage stereo;
    stereo.prepare (rate);
    stereo.setTarget ({ 0.25f, 4.0f });
    auto both = ones (1000);
    process (stereo, both, 0, 1000);

    GainStage mono;
    mono.prepare (rate);
    mono.setTarget ({ 0.25f, 4.0f });
    std::vector<float> left (1000, 1.0f);
    mono.process (left.data(), nullptr, 400);
    mono.process (left.data() + 400, nullptr, 600);

    CHECK (left == both.left);
    CHECK (mono.current() == stereo.current());
}

TEST_CASE ("gain stage: both gains are handed over as one value, never read torn")
{
    // One thread sets pairs whose two gains are always equal, as fast as it
    // can; another reads them. A torn read would have one channel's new gain
    // with the other's old.
    GainStage stage;
    std::atomic<bool> stop { false };
    std::atomic<std::uint64_t> stored { 0 };

    std::thread writer ([&stage, &stop, &stored]
    {
        for (std::uint32_t i = 1; ! stop.load (std::memory_order_relaxed); ++i)
        {
            const auto gain = static_cast<float> (i % 1000000u) * 0.001f;
            stage.setTarget ({ gain, gain });
            stored.fetch_add (1, std::memory_order_relaxed);
        }
    });

    std::uint64_t reads = 0;
    std::uint64_t torn = 0;
    std::uint64_t changed = 0;
    auto last = stage.target();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (300);

    while (std::chrono::steady_clock::now() < until)
    {
        for (int i = 0; i < 1000; ++i)
        {
            const auto read = stage.target();
            torn += read.left != read.right ? 1 : 0;
            changed += read != last ? 1 : 0;
            last = read;
            ++reads;
        }
    }

    stop = true;
    writer.join();

    MESSAGE ("gain stage: " << reads << " reads, " << changed << " of them new pairs, while " << stored.load() << " were stored");
    CHECK (torn == 0);
    CHECK (changed > 1000); // the two threads really did overlap
}

TEST_CASE ("gain stage: nothing is allocated or freed on the audio thread")
{
    GainStage stage;
    stage.prepare (rate);
    auto audio = ones (20000);

    const testing::CountAllocations count;

    for (int block = 0; block < 78; ++block)
    {
        if (block % 5 == 0)
            stage.setTarget ({ static_cast<float> (block % 3) * 0.5f, 1.0f - static_cast<float> (block % 2) });

        process (stage, audio, block * 256, 256);
    }

    CHECK (count.allocations() == 0);
    CHECK (count.frees() == 0);
}
