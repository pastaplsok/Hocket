#include "engine/mix/GainStage.h"

#include <algorithm>
#include <cmath>

namespace hocket::engine
{
int GainStage::rampSamples (double sampleRate) noexcept
{
    return std::max (1, static_cast<int> (std::floor (rampSeconds * sampleRate)));
}

std::uint64_t GainStage::pack (StereoGain gain) noexcept
{
    return static_cast<std::uint64_t> (std::bit_cast<std::uint32_t> (gain.left))
         | (static_cast<std::uint64_t> (std::bit_cast<std::uint32_t> (gain.right)) << 32);
}

StereoGain GainStage::unpack (std::uint64_t bits) noexcept
{
    return { std::bit_cast<float> (static_cast<std::uint32_t> (bits)), std::bit_cast<float> (static_cast<std::uint32_t> (bits >> 32)) };
}

//==============================================================================
void GainStage::setTarget (StereoGain gain) noexcept
{
    packedTarget.store (pack (gain), std::memory_order_release);
}

StereoGain GainStage::target() const noexcept
{
    return unpack (packedTarget.load (std::memory_order_acquire));
}

void GainStage::prepare (double sampleRate) noexcept
{
    rampLength = static_cast<std::uint32_t> (rampSamples (sampleRate));
    rampLeft = 0;
    to = target();
    from = to;
    now = to;
}

//==============================================================================
void GainStage::process (float* left, float* right, int numSamples) noexcept
{
    // Both channels' targets in one load. A new pair, even mid-ramp, starts a
    // new ramp from where the gains are.
    if (const auto latest = target(); latest != to)
    {
        from = now;
        to = latest;
        rampLeft = rampLength;
    }

    int i = 0;

    // Sample k of the ramp, from 1 to rampLength, gets from + (to - from) * k
    // / rampLength, and the last exactly `to`: both channels by one count, so
    // they arrive together.
    for (; i < numSamples && rampLeft > 0; ++i)
    {
        --rampLeft;

        if (rampLeft == 0)
        {
            now = to;
        }
        else
        {
            const auto k = static_cast<float> (rampLength - rampLeft) / static_cast<float> (rampLength);
            now = { from.left + (to.left - from.left) * k, from.right + (to.right - from.right) * k };
        }

        left[i] *= now.left;

        if (right != nullptr)
            right[i] *= now.right;
    }

    // Settled: exactly the target.
    for (int j = i; j < numSamples; ++j)
        left[j] *= to.left;

    if (right != nullptr)
        for (int j = i; j < numSamples; ++j)
            right[j] *= to.right;
}
}
