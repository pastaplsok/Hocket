#pragma once

// Writing WAV files for tests: plain headers, 16- or 24-bit PCM or 32-bit
// float, from samples between -1 and 1 (float may go beyond).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hocket::testing
{
    enum class WavEncoding
    {
        pcm16,
        pcm24,
        float32
    };

    inline void writeWav (const std::filesystem::path& file, const std::vector<std::vector<float>>& channels, double rate,
                          WavEncoding encoding = WavEncoding::pcm16)
    {
        const auto bits = encoding == WavEncoding::pcm16 ? 16u : encoding == WavEncoding::pcm24 ? 24u : 32u;
        const auto frames = channels.front().size();
        const auto block = static_cast<std::uint32_t> (channels.size() * bits / 8);
        const auto dataBytes = static_cast<std::uint32_t> (frames * block);

        std::string bytes;
        const auto put = [&bytes] (std::uint64_t value, int count)
        {
            for (int i = 0; i < count; ++i)
                bytes += static_cast<char> ((value >> (8 * i)) & 0xff);
        };

        bytes += "RIFF";
        put (36 + dataBytes, 4);
        bytes += "WAVEfmt ";
        put (16, 4);
        put (encoding == WavEncoding::float32 ? 3 : 1, 2);
        put (channels.size(), 2);
        put (static_cast<std::uint32_t> (rate), 4);
        put (static_cast<std::uint32_t> (rate) * block, 4);
        put (block, 2);
        put (bits, 2);
        bytes += "data";
        put (dataBytes, 4);

        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            for (const auto& channel : channels)
            {
                const auto value = channel[frame];

                if (encoding == WavEncoding::float32)
                {
                    std::uint32_t raw = 0;
                    std::memcpy (&raw, &value, 4);
                    put (raw, 4);
                }
                else
                {
                    const auto full = encoding == WavEncoding::pcm16 ? 32767.0 : 8388607.0;
                    const auto n = static_cast<std::int64_t> (std::lround (std::clamp (static_cast<double> (value), -1.0, 1.0) * full));
                    put (static_cast<std::uint64_t> (n), encoding == WavEncoding::pcm16 ? 2 : 3);
                }
            }
        }

        std::filesystem::create_directories (file.parent_path());
        std::ofstream out (file, std::ios::binary);
        out << bytes;
    }
}
