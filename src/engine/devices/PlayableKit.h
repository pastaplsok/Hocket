#pragma once

// A kit as Hocket's drum sampler plays it (SPEC §8.1, §8.4): each piece's
// audio, already at the playback rate, its gain, and its choke group, found
// by MIDI note. Built on the message thread (or the loader thread, from
// step 7's part 4) and never changed after, so the audio thread reads it
// without locks. It's freed on the message thread only (DrumVoices).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hocket::engine
{
    // One sample's audio at the playback rate: one channel or two, of equal length.
    struct SampleAudio
    {
        std::vector<std::vector<float>> channels;

        std::size_t length() const { return channels.empty() ? 0 : channels.front().size(); }
    };

    class PlayableKit
    {
    public:
        struct Piece
        {
            int note = 0;                             // MIDI note number, 0 to 127; one piece per note
            std::shared_ptr<const SampleAudio> audio; // shared with the cache of converted samples
            float gain = 1.0f;                        // the piece's level, as a gain
            int chokeGroup = -1;                      // its group in the kit; -1 for none
        };

        // What the audio thread reads for a piece. Pointers into `audio`.
        struct Voicing
        {
            const float* left = nullptr;
            const float* right = nullptr; // the same as left for a mono sample
            std::size_t length = 0;
            float gain = 1.0f;
            int chokeGroup = -1;
        };

        // sampleRate is the rate the audio is at, which must be the rate it
        // plays at. Throws std::invalid_argument for a note out of range or
        // used twice, or audio with no channels, more than two, or channels
        // of different lengths.
        PlayableKit (double sampleRate, std::vector<Piece> pieces);

        // A kit with no pieces: silence, for a track whose kit can't play.
        static std::unique_ptr<PlayableKit> silent (double sampleRate);

        double sampleRate() const { return rate; }

        // The piece on this note, or nullptr. Real-time safe.
        const Voicing* pieceOn (int note) const noexcept
        {
            return note >= 0 && note < 128 && byNote[static_cast<std::size_t> (note)] >= 0
                     ? &voicings[static_cast<std::size_t> (byNote[static_cast<std::size_t> (note)])]
                     : nullptr;
        }

        // Set by DrumVoices when it's handed over: newer kits have higher numbers.
        std::uint64_t generation = 0;

    private:
        double rate = 0.0;
        std::vector<Piece> pieces;
        std::vector<Voicing> voicings;
        std::array<int, 128> byNote {};
    };
}
