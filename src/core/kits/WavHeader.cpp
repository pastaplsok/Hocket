#include "core/kits/WavHeader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace hocket::kits
{
namespace
{
    // Little-endian, as RIFF is.
    std::uint32_t read32 (const unsigned char* bytes)
    {
        return static_cast<std::uint32_t> (bytes[0]) | static_cast<std::uint32_t> (bytes[1]) << 8 | static_cast<std::uint32_t> (bytes[2]) << 16
             | static_cast<std::uint32_t> (bytes[3]) << 24;
    }

    std::uint16_t read16 (const unsigned char* bytes)
    {
        return static_cast<std::uint16_t> (bytes[0] | bytes[1] << 8);
    }

    bool readBytes (std::istream& in, unsigned char* into, std::size_t count)
    {
        in.read (reinterpret_cast<char*> (into), static_cast<std::streamsize> (count));
        return static_cast<std::size_t> (in.gcount()) == count;
    }

    constexpr int formatPcm = 1;
    constexpr int formatFloat = 3;
    constexpr int formatExtensible = 0xfffe;

    // The extensible format's subformat GUIDs all end the same way; the first
    // two bytes are the ordinary format tag.
    constexpr std::array<unsigned char, 14> guidTail { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };

    // The "fmt " chunk's contents.
    bool readFormat (const unsigned char* body, std::uint32_t length, WavHeader& header)
    {
        if (length < 16)
            return false;

        auto tag = static_cast<int> (read16 (body));
        header.channels = read16 (body + 2);
        header.sampleRate = read32 (body + 4);
        header.bitsPerSample = read16 (body + 14);

        if (tag == formatExtensible)
        {
            if (length < 40 || read16 (body + 16) < 22)
                return false;

            if (const auto valid = read16 (body + 18); valid != 0)
                header.bitsPerSample = valid;

            tag = std::memcmp (body + 26, guidTail.data(), guidTail.size()) == 0 ? static_cast<int> (read16 (body + 24)) : -1;
        }

        header.formatTag = tag;
        header.encoding = tag == formatPcm ? WavHeader::Encoding::pcm : tag == formatFloat ? WavHeader::Encoding::ieeeFloat : WavHeader::Encoding::other;
        return header.channels > 0 && header.sampleRate > 0;
    }
}

std::optional<WavHeader> readWavHeader (std::istream& in, std::uint64_t size)
{
    unsigned char riff[12];

    if (size < 12 || ! readBytes (in, riff, 12) || std::memcmp (riff, "RIFF", 4) != 0 || std::memcmp (riff + 8, "WAVE", 4) != 0)
        return {}; // RF64 and other relatives included: the decoder decides

    WavHeader header;
    bool haveFormat = false;
    std::optional<std::uint64_t> dataLength;
    std::uint16_t blockAlign = 0;
    std::uint64_t position = 12;

    // Chunks in any order, to the end of the file (the RIFF size is often
    // wrong), each skipped by its size, as the decoder skips them. A missing
    // pad byte is allowed only at the end of the file.
    while (position + 8 <= size && ! (haveFormat && dataLength))
    {
        unsigned char chunk[8];

        if (! readBytes (in, chunk, 8))
            return {};

        const auto length = read32 (chunk + 4);
        const auto bodyStart = position + 8;
        const auto available = size - bodyStart;

        if (std::memcmp (chunk, "data", 4) == 0)
        {
            // A size past the end (0xFFFFFFFF from a recorder that never went
            // back to fill it in, say) can't be trusted.
            dataLength = length <= available ? std::optional<std::uint64_t> (length) : std::optional<std::uint64_t> (0xffffffffffffffffull);
        }
        else if (length > available)
        {
            return {};
        }
        else if (std::memcmp (chunk, "fmt ", 4) == 0)
        {
            std::array<unsigned char, 64> body {};
            const auto toRead = std::min<std::uint32_t> (length, static_cast<std::uint32_t> (body.size()));

            if (! readBytes (in, body.data(), toRead) || ! readFormat (body.data(), length, header))
                return {};

            blockAlign = read16 (body.data() + 12);
            haveFormat = true;
        }

        // The next chunk starts after this one's pad byte, if its length is odd.
        position = bodyStart + length + (length & 1u);

        if (dataLength && *dataLength == 0xffffffffffffffffull)
            break; // nothing after it can be found

        in.clear();
        in.seekg (static_cast<std::streamoff> (position));
    }

    if (! haveFormat || ! dataLength)
        return {};

    if (*dataLength != 0xffffffffffffffffull && blockAlign > 0)
        header.frames = *dataLength / blockAlign;

    return header;
}

std::optional<WavHeader> readWavHeader (const std::filesystem::path& file)
{
    std::error_code error;
    const auto size = std::filesystem::file_size (file, error);

    if (error)
        return {};

    std::ifstream in (file, std::ios::binary);

    if (! in)
        return {};

    return readWavHeader (in, size);
}

std::optional<std::string> sampleFormatProblem (const WavHeader& header)
{
    using Encoding = WavHeader::Encoding;
    const auto supported = (header.encoding == Encoding::pcm && (header.bitsPerSample == 16 || header.bitsPerSample == 24))
                        || (header.encoding == Encoding::ieeeFloat && header.bitsPerSample == 32);

    if (! supported)
    {
        const auto what = header.encoding == Encoding::pcm         ? std::to_string (header.bitsPerSample) + "-bit PCM"
                        : header.encoding == Encoding::ieeeFloat ? std::to_string (header.bitsPerSample) + "-bit float"
                                                                  : "a compressed or unusual format (format " + std::to_string (header.formatTag) + ")";
        return "it's " + what + "; kits' samples are 16- or 24-bit PCM, or 32-bit float, so export it as one of those";
    }

    if (header.channels > 2)
        return "it has " + std::to_string (header.channels) + " channels; a kit's samples are mono or stereo";

    if (header.frames)
    {
        const auto seconds = static_cast<double> (*header.frames) / header.sampleRate;

        if (seconds > maxSampleSeconds)
        {
            const auto tenths = std::round (seconds * 10.0) / 10.0;
            auto text = std::to_string (tenths);
            text.erase (text.find_last_not_of ('0') + 1);

            if (text.ends_with ('.'))
                text.pop_back();

            return "it's " + text + " s long; a kit's samples are one-shots of at most 10 s";
        }
    }

    return {};
}
}
