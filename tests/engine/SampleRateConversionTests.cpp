// Converting a kit's samples to the device's rate (SPEC §8.1), through the
// libsamplerate Tracktion bundles. The targets (Lee, 2026-11-17): flat to
// 20 kHz within 0.1 dB, aliasing at least 90 dB down, and a hit where it was.

#include "engine/kits/SampleRateConversion.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>

using namespace hocket;

namespace
{
    constexpr double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };

    std::vector<float> sine (double frequency, double rate, double seconds, double amplitude = 0.5)
    {
        std::vector<float> samples (static_cast<std::size_t> (seconds * rate));

        for (std::size_t n = 0; n < samples.size(); ++n)
            samples[n] = static_cast<float> (amplitude * std::sin (2.0 * std::numbers::pi * frequency * static_cast<double> (n) / rate));

        return samples;
    }

    // A sine at a known frequency fitted to the middle of the samples (away
    // from the ends, where the filter meets silence): its amplitude, its
    // phase at sample 0, and what's left over, as an RMS level.
    struct Fit
    {
        double amplitude = 0.0;
        double phase = 0.0;
        double residualRms = 0.0;
    };

    Fit fitSine (const std::vector<float>& samples, double frequency, double rate, std::size_t margin)
    {
        // Least squares for a·sin + b·cos.
        double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;

        for (std::size_t n = margin; n + margin < samples.size(); ++n)
        {
            const auto w = 2.0 * std::numbers::pi * frequency * static_cast<double> (n) / rate;
            const auto s = std::sin (w), c = std::cos (w);
            ss += s * s, cc += c * c, sc += s * c, xs += samples[n] * s, xc += samples[n] * c;
        }

        const auto det = ss * cc - sc * sc;
        const auto a = (xs * cc - xc * sc) / det;
        const auto b = (xc * ss - xs * sc) / det;

        double residual = 0;
        std::size_t count = 0;

        for (std::size_t n = margin; n + margin < samples.size(); ++n, ++count)
        {
            const auto w = 2.0 * std::numbers::pi * frequency * static_cast<double> (n) / rate;
            const auto left = samples[n] - (a * std::sin (w) + b * std::cos (w));
            residual += left * left;
        }

        return { std::hypot (a, b), std::atan2 (b, a), std::sqrt (residual / static_cast<double> (count)) };
    }

    double rms (const std::vector<float>& samples, std::size_t margin)
    {
        double sum = 0;
        std::size_t count = 0;

        for (std::size_t n = margin; n + margin < samples.size(); ++n, ++count)
            sum += static_cast<double> (samples[n]) * samples[n];

        return std::sqrt (sum / static_cast<double> (count));
    }

    double dB (double ratio)
    {
        return 20.0 * std::log10 (ratio);
    }
}

TEST_CASE ("sample rates: equal rates leave the samples bit for bit, and the length follows the ratio")
{
    std::mt19937 random (7);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
    std::vector<float> samples (1001);

    for (auto& sample : samples)
        sample = noise (random);

    CHECK (engine::convertSampleRate (samples, 48000.0, 48000.0) == samples);
    CHECK (engine::convertSampleRate ({}, 48000.0, 44100.0).empty());

    CHECK (engine::convertSampleRate (samples, 48000.0, 44100.0).size() == 920); // 919.6875
    CHECK (engine::convertSampleRate (samples, 44100.0, 96000.0).size() == 2179); // 2179.05
    CHECK (engine::convertSampleRate (std::vector<float> (48000), 48000.0, 44100.0).size() == 44100);

    CHECK_THROWS_AS (engine::convertSampleRate (samples, 0.0, 44100.0), std::runtime_error);
    CHECK_THROWS_AS (engine::convertSampleRate (samples, 48000.0, -1.0), std::runtime_error);
}

TEST_CASE ("sample rates: a tone keeps its level and timing to 20 kHz, with nothing else added")
{
    // Measured with libsamplerate's best converter on 2026-11-17: level within
    // 0.000002 dB, residual -139 dB. Its medium one fails: 3.3 dB down at 20 kHz.
    double worstLevel = 0.0;
    double worstResidual = -300.0;

    for (const auto from : rates)
    {
        for (const auto to : rates)
        {
            if (from == to)
                continue;

            for (const auto frequency : { 100.0, 1000.0, 10000.0, 20000.0 })
            {
                INFO (from << " Hz to " << to << " Hz, a " << frequency << " Hz tone");
                const auto converted = engine::convertSampleRate (sine (frequency, from, 0.5), from, to);
                const auto fit = fitSine (converted, frequency, to, static_cast<std::size_t> (to / 20)); // 50 ms from each end
                const auto level = dB (fit.amplitude / 0.5);
                const auto residual = dB (fit.residualRms / (0.5 / std::numbers::sqrt2));

                // Level within 0.1 dB, and in phase with the original: a time
                // offset under 0.01 of a sample at the new rate.
                CHECK (std::abs (level) < 0.1);
                CHECK (std::abs (fit.phase / (2.0 * std::numbers::pi * frequency) * to) < 0.01);

                // What's left once the tone is taken out: images and noise, at least 90 dB down.
                CHECK_MESSAGE (residual < -90.0, "residual " << residual << " dB");

                worstLevel = std::max (worstLevel, std::abs (level));
                worstResidual = std::max (worstResidual, residual);
            }
        }
    }

    MESSAGE ("worst level error " << worstLevel << " dB, worst residual " << worstResidual << " dB");
}

TEST_CASE ("sample rates: a tone above the lower rate's Nyquist frequency is filtered out, not folded back")
{
    struct Case
    {
        double from;
        double to;
        double frequency;
    };

    // Measured on 2026-11-17: -141 to -152 dB.
    double worst = -300.0;

    for (const auto [from, to, frequency] : { Case { 48000.0, 44100.0, 23000.0 }, Case { 88200.0, 44100.0, 30000.0 }, Case { 96000.0, 44100.0, 40000.0 },
                                             Case { 96000.0, 48000.0, 26000.0 }, Case { 88200.0, 48000.0, 30000.0 } })
    {
        INFO (from << " Hz to " << to << " Hz, a " << frequency << " Hz tone");
        const auto converted = engine::convertSampleRate (sine (frequency, from, 0.5), from, to);
        const auto level = dB (rms (converted, static_cast<std::size_t> (to / 20)) / (0.5 / std::numbers::sqrt2));
        CHECK_MESSAGE (level < -90.0, "what came through: " << level << " dB");
        worst = std::max (worst, level);
    }

    MESSAGE ("the loudest that came through: " << worst << " dB");
}

TEST_CASE ("sample rates: a click stays where it was")
{
    for (const auto from : rates)
    {
        for (const auto to : rates)
        {
            INFO (from << " Hz to " << to << " Hz");

            // A click 0.1 s in: where its energy is, after.
            std::vector<float> click (static_cast<std::size_t> (from * 0.2));
            click[static_cast<std::size_t> (from * 0.1)] = 1.0f;

            const auto converted = engine::convertSampleRate (click, from, to);
            double energy = 0, weighted = 0;

            for (std::size_t n = 0; n < converted.size(); ++n)
            {
                const auto e = static_cast<double> (converted[n]) * converted[n];
                energy += e;
                weighted += e * static_cast<double> (n);
            }

            CHECK (std::abs (weighted / energy - to * 0.1) < 0.05);
        }
    }
}

TEST_CASE ("sample rates: how long a kit's worth of conversion takes")
{
    // About a kit's worth: 20 pieces of a second, in stereo, at 48 kHz to 44.1 kHz.
    std::mt19937 random (11);
    std::uniform_real_distribution<float> noise (-0.5f, 0.5f);
    std::vector<float> second (48000);

    for (auto& sample : second)
        sample = noise (random);

    const auto start = std::chrono::steady_clock::now();

    for (int channel = 0; channel < 40; ++channel)
        CHECK (engine::convertSampleRate (second, 48000.0, 44100.0).size() == 44100);

    const auto took = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    MESSAGE ("40 seconds of samples, 48 kHz to 44.1 kHz: " << took << " ms");
}
