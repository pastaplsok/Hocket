#pragma once

// The real-time core of Hocket's drum sampler (SPEC §5.2, §8.1): hits
// played from a PlayableKit, as one-shots, with choke groups. No Tracktion
// here; the plugin (step 7, part 4) turns MIDI into DrumEvents.
//
// Two threads use it, and nothing on the audio thread locks or allocates:
// - The message thread hands over kits (setKit), frees the ones no hit uses
//   any more (collectRetired), and asks for silence (requestSilence).
// - The audio thread renders blocks (process): one thread at a time, though
//   not always the same one (Tracktion's pool).
// A kit is handed over through one atomic pointer, which the audio thread
// takes at the start of a block. Each kit carries a number; the audio thread
// publishes the oldest one any sounding hit still plays from, and the
// message thread frees only kits older than that, so hits ring on through a
// kit change. Voices are a fixed pool, the audio thread's alone.

#include "engine/devices/PlayableKit.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hocket::engine
{
    // A hit's gain at a velocity: (velocity / 127)², General MIDI's default
    // curve (v100 is -4.2 dB, v64 -11.9 dB, v28 -26 dB). The one place it's
    // defined (SPEC §8.1), so it can be tuned by ear.
    constexpr float velocityGain (int velocity) noexcept
    {
        const auto v = static_cast<float> (velocity < 0 ? 0 : velocity > 127 ? 127 : velocity) / 127.0f;
        return v * v;
    }

    struct DrumEvent
    {
        enum class Kind
        {
            hit,    // a note-on: the piece on `note` starts at `offset`
            silence // every hit fades out (MIDI's all-sound-off); note-offs and all-notes-off are ignored, as one-shots ring on
        };

        Kind kind = Kind::hit;
        int offset = 0; // samples into the block
        int note = 0;
        int velocity = 0; // 1 to 127; 0 is a note-off, and ignored
    };

    class DrumVoices
    {
    public:
        static constexpr std::size_t maxVoices = 64;

        // When fewer voices than this are free, the oldest hit still at full
        // level fades out to make room, so a new hit finds a free voice, and
        // none is cut short without a fade, short of hundreds of hits a
        // second (Counts::cutShort). A hit is never dropped.
        static constexpr std::size_t voicesKeptFree = 8;

        // How long a choke, or silence, takes to fade a hit out.
        static constexpr double fadeSeconds = 0.010;

        DrumVoices();
        ~DrumVoices();

        DrumVoices (const DrumVoices&) = delete;
        DrumVoices& operator= (const DrumVoices&) = delete;

        //==============================================================================
        // Message thread.

        // Hands a kit to the audio thread, which plays it from its next block.
        // A kit handed over before the audio thread took the last one replaces
        // it, and that one is freed at once.
        void setKit (std::unique_ptr<PlayableKit> kit);

        // Frees kits that no hit plays from any more, and that the audio
        // thread has moved on from. Call it now and then, from a timer.
        void collectRetired();

        // Every hit fades out, at the start of the audio thread's next block
        // (Tracktion's reset and MIDI panic).
        void requestSilence() noexcept;

        // How many kits are held: the current one, ones hits still play
        // from, and one handed over but not yet taken. For tests.
        std::size_t kitsHeld() const { return owned.size(); }

        //==============================================================================
        // Audio thread. Real-time safe.

        // Adds the block's hits into left and right (which the caller clears
        // first, if need be). Events are in order of offset; one out of order
        // is played at the place the block has reached.
        void process (float* left, float* right, int numSamples, std::span<const DrumEvent> events) noexcept;

        // Counts, for tests: hits started; voices taken before they had faded
        // out. That happens only when hits come faster than fades free voices:
        // with the pool full of ringing hits, more than voicesKeptFree within
        // one fade (10 ms), so above several hundred a second.
        struct Counts
        {
            std::uint64_t hits = 0;
            std::uint64_t cutShort = 0;
        };

        // Written by the audio thread; read only while it isn't running.
        const Counts& counts() const { return audioCounts; }

    private:
        struct Voice
        {
            const PlayableKit::Voicing* piece = nullptr; // nullptr: free
            std::uint64_t generation = 0;                // of the kit it plays from
            std::uint64_t started = 0;                   // order of starting, for the oldest
            std::size_t position = 0;
            float gain = 0.0f;
            std::uint32_t fadeLength = 0; // in samples; 0 while not fading
            std::uint32_t fadeLeft = 0;   // the fade's gain is fadeLeft / fadeLength, down by one each sample

            // 1 at full level, falling to 0 as it fades out.
            float level() const noexcept { return fadeLength == 0 ? 1.0f : static_cast<float> (fadeLeft) / static_cast<float> (fadeLength); }
        };

        // Message thread's.
        std::vector<std::unique_ptr<PlayableKit>> owned; // oldest first
        std::uint64_t nextGeneration = 1;

        // Shared.
        std::atomic<PlayableKit*> pending { nullptr };
        std::atomic<std::uint64_t> oldestInUse { 0 }; // 0 until the audio thread has a kit
        std::atomic<bool> silenceRequested { false };

        static_assert (std::atomic<PlayableKit*>::is_always_lock_free);
        static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert (std::atomic<bool>::is_always_lock_free);

        // Audio thread's.
        const PlayableKit* current = nullptr;
        std::array<Voice, maxVoices> voices {};
        std::uint64_t nextStart = 1;
        Counts audioCounts;

        void startHit (const DrumEvent& event) noexcept;
        void fadeOut (Voice& voice) const noexcept;
        void render (float* left, float* right, int from, int to) noexcept;
        void publishOldestInUse() noexcept;
    };
}
