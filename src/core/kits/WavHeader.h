#pragma once

// What a kit's WAV file holds, read from its header alone (SPEC §8.4), so a
// sample Hocket can't play is caught when the kit is read, before anything
// decodes it. It reads the files real sample packs hold: the extensible format
// header, and other chunks (LIST, smpl, acid, bext, JUNK...) before and after
// the audio, odd-length ones with their pad byte. A file it can't make sense
// of isn't refused here: the decoder decides.

#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>

namespace hocket::kits
{
    struct WavHeader
    {
        enum class Encoding
        {
            pcm,        // integer samples
            ieeeFloat,  // floating-point samples
            other       // compressed, or anything else
        };

        Encoding encoding = Encoding::other;
        int formatTag = 0;     // as written; for the extensible format, its subformat's
        int bitsPerSample = 0; // the bits that carry the sample (the extensible format's valid bits)
        int channels = 0;
        double sampleRate = 0.0;
        std::optional<std::uint64_t> frames; // none when the data chunk's size can't be trusted
    };

    // The header of the WAV file `size` bytes long that `in` reads from the
    // start, or nullopt when it can't make sense of it (not RIFF WAVE, RF64, a
    // chunk running past the end, no "fmt " or "data" chunk...).
    std::optional<WavHeader> readWavHeader (std::istream& in, std::uint64_t size);
    std::optional<WavHeader> readWavHeader (const std::filesystem::path& file);

    // Why a kit can't use this sample, for a reader of the kit's manifest:
    // 16- or 24-bit PCM, or 32-bit float, one or two channels, at most
    // maxSampleSeconds long. Nothing when the header can't tell (no frames).
    inline constexpr double maxSampleSeconds = 10.0;
    std::optional<std::string> sampleFormatProblem (const WavHeader& header);
}
