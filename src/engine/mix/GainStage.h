#pragma once

// How a track's mix is played (SPEC §4.4): a gain on each channel, which the
// message thread sets and the audio thread ramps to. No Tracktion here;
// GainStagePlugin puts one on every track.
//
// The two gains are handed over as one atomic value, so the audio thread
// never sees one channel's new gain with the other's old: a block that heads
// for a gain no setting asks for is a level bump (PROGRESS.md, step 7, "Before
// 4b"). When the pair changes, both channels go in a straight line from where
// they are to the new pair over 15 ms, together, counted in whole samples, so
// the output is the same however the blocks fall.
//
// Nothing on the audio thread locks or allocates.

#include <atomic>
#include <bit>
#include <cstdint>

namespace hocket::engine
{
    // A gain on each channel: 1 leaves it as it is, 0 silences it.
    struct StereoGain
    {
        float left = 1.0f;
        float right = 1.0f;

        friend bool operator== (StereoGain, StereoGain) = default;
    };

    class GainStage
    {
    public:
        // As long as Tracktion's volume plugin's smoothing (VolumeAndPanPlugin).
        static constexpr double rampSeconds = 0.015;

        // How many samples a ramp takes at a rate: 661 at 44.1 kHz, counted as
        // juce::SmoothedValue counts them.
        static int rampSamples (double sampleRate) noexcept;

        GainStage() = default;

        GainStage (const GainStage&) = delete;
        GainStage& operator= (const GainStage&) = delete;

        //==============================================================================
        // Any thread; the message thread sets it.

        // The pair the audio thread ramps to, from its next block.
        void setTarget (StereoGain) noexcept;
        StereoGain target() const noexcept;

        //==============================================================================
        // While no audio thread runs it: when a graph starts playing it.

        // Sets the ramp's length for this rate, and puts the gains at the
        // target, with no ramp.
        void prepare (double sampleRate) noexcept;

        //==============================================================================
        // Audio thread. Real-time safe.

        // Applies the gains to a block, ramping to a new target if there is
        // one. `right` may be null (a mono block): the ramp moves on all the same.
        void process (float* left, float* right, int numSamples) noexcept;

        // The gains the last sample processed got; for tests, while no audio
        // thread runs it.
        StereoGain current() const noexcept { return now; }

    private:
        static std::uint64_t pack (StereoGain) noexcept;
        static StereoGain unpack (std::uint64_t) noexcept;

        // Shared: the target pair, as the bits of two floats.
        std::atomic<std::uint64_t> packedTarget { pack ({}) };
        static_assert (std::atomic<std::uint64_t>::is_always_lock_free);

        // The audio thread's (and prepare's).
        StereoGain from;        // where the ramp started
        StereoGain to;          // where it goes, and stays
        StereoGain now;         // the last sample's
        std::uint32_t rampLength = 1;
        std::uint32_t rampLeft = 0; // samples to go; 0 when settled
    };
}
