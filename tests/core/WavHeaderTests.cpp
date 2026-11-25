// Reading a kit's WAV headers (SPEC §8.4) from files laid out as real sample
// packs lay them out (tests/fixtures/wav, made by make-fixtures.py). What the
// header rules out is refused; what it can't make sense of is left to the decoder.

#include "core/Paths.h"
#include "core/kits/WavHeader.h"

#include <doctest/doctest.h>

#include <cstring>
#include <sstream>

using namespace hocket;
using kits::WavHeader;
using Encoding = WavHeader::Encoding;

namespace
{
    std::filesystem::path fixture (const char* name)
    {
        return pathFromUtf8 (HOCKET_FIXTURES_DIR) / pathFromUtf8 ("wav") / pathFromUtf8 (name);
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    void put16 (std::string& bytes, unsigned value)
    {
        bytes += static_cast<char> (value & 0xff);
        bytes += static_cast<char> ((value >> 8) & 0xff);
    }

    void put32 (std::string& bytes, std::uint32_t value)
    {
        put16 (bytes, value & 0xffff);
        put16 (bytes, value >> 16);
    }

    // A plain PCM header whose data chunk says it holds `dataBytes`; the
    // bytes themselves aren't there, as the reader never reads them.
    std::string pcmHeader (unsigned channels, std::uint32_t rate, unsigned bits, std::uint32_t dataBytes)
    {
        std::string bytes = "RIFF";
        put32 (bytes, 36 + dataBytes);
        bytes += "WAVEfmt ";
        put32 (bytes, 16);
        put16 (bytes, 1);
        put16 (bytes, channels);
        put32 (bytes, rate);
        put32 (bytes, rate * channels * bits / 8);
        put16 (bytes, channels * bits / 8);
        put16 (bytes, bits);
        bytes += "data";
        put32 (bytes, dataBytes);
        return bytes;
    }

    std::optional<WavHeader> readBytes (const std::string& bytes, std::uint64_t size)
    {
        std::istringstream in (bytes, std::ios::binary);
        return kits::readWavHeader (in, size);
    }
}

TEST_CASE ("WAV headers: the formats a kit takes, in the layouts sample packs use")
{
    struct Case
    {
        const char* file;
        Encoding encoding;
        int bits;
        int channels;
        double rate;
    };

    const Case cases[] = {
        { "pcm16-mono.wav", Encoding::pcm, 16, 1, 44100 },
        { "pcm24-stereo.wav", Encoding::pcm, 24, 2, 44100 },
        { "float32-stereo.wav", Encoding::ieeeFloat, 32, 2, 44100 },
        { "float32-loud.wav", Encoding::ieeeFloat, 32, 1, 44100 },
        { "extensible-pcm24.wav", Encoding::pcm, 24, 2, 44100 },
        { "extensible-pcm24-in-32.wav", Encoding::pcm, 24, 1, 44100 }, // 24 valid bits in 32-bit containers
        { "extensible-float32-48k.wav", Encoding::ieeeFloat, 32, 2, 48000 },
        { "chunks-around.wav", Encoding::pcm, 16, 2, 44100 }, // JUNK, bext, odd-length LIST, smpl, acid, no final pad byte
    };

    for (const auto& c : cases)
    {
        INFO (c.file);
        const auto header = kits::readWavHeader (fixture (c.file));
        REQUIRE (header);
        CHECK (header->encoding == c.encoding);
        CHECK (header->bitsPerSample == c.bits);
        CHECK (header->channels == c.channels);
        CHECK (header->sampleRate == c.rate);
        CHECK (header->frames == 32u);
        CHECK_FALSE (kits::sampleFormatProblem (*header));
    }
}

TEST_CASE ("WAV headers: what a kit can't use is refused, saying why")
{
    const auto eightBit = kits::readWavHeader (fixture ("pcm8-mono.wav"));
    REQUIRE (eightBit);
    CHECK (kits::sampleFormatProblem (*eightBit)
           == "it's 8-bit PCM; kits' samples are 16- or 24-bit PCM, or 32-bit float, so export it as one of those");

    const auto threeChannels = kits::readWavHeader (fixture ("three-channels.wav"));
    REQUIRE (threeChannels);
    CHECK (kits::sampleFormatProblem (*threeChannels) == "it has 3 channels; a kit's samples are mono or stereo");

    // 10.5 s at 8 kHz, mono, 16-bit: the header alone says so.
    const auto tooLong = readBytes (pcmHeader (1, 8000, 16, 168000), 44 + 168000);
    REQUIRE (tooLong);
    CHECK (tooLong->frames == 84000u);
    CHECK (kits::sampleFormatProblem (*tooLong) == "it's 10.5 s long; a kit's samples are one-shots of at most 10 s");

    const auto tenSeconds = readBytes (pcmHeader (1, 8000, 16, 160000), 44 + 160000);
    REQUIRE (tenSeconds);
    CHECK_FALSE (kits::sampleFormatProblem (*tenSeconds));

    // 32-bit integer PCM and compressed formats.
    auto int32 = pcmHeader (1, 44100, 32, 128);
    CHECK (kits::sampleFormatProblem (*readBytes (int32, int32.size() + 128)) == "it's 32-bit PCM; kits' samples are 16- or 24-bit PCM, or 32-bit "
                                                                                 "float, so export it as one of those");
    auto adpcm = pcmHeader (1, 44100, 4, 128);
    adpcm[20] = 2; // the format tag: Microsoft ADPCM
    CHECK (contains (*kits::sampleFormatProblem (*readBytes (adpcm, adpcm.size() + 128)), "it's a compressed or unusual format (format 2)"));
}

// Bug sweep, area 2, finding 3: libsamplerate takes ratios from 1/256 to 256,
// and a sample at a rate it couldn't take ended the process as its kit loaded.
TEST_CASE ("WAV headers: a sample rate outside 8 to 384 kHz is refused, saying why" * doctest::should_fail())
{
    for (const std::uint32_t rate : { 8000u, 11025u, 44100u, 192000u, 384000u })
    {
        CAPTURE (rate);
        auto header = pcmHeader (1, rate, 16, 128);
        CHECK_FALSE (kits::sampleFormatProblem (*readBytes (header, header.size() + 128)));
    }

    for (const std::uint32_t rate : { 100u, 7999u, 384001u })
    {
        CAPTURE (rate);
        auto header = pcmHeader (1, rate, 16, 128);
        const auto problem = kits::sampleFormatProblem (*readBytes (header, header.size() + 128));
        CHECK (problem == "it's at " + std::to_string (rate) + " Hz; kits' samples are at 8 to 384 kHz, so export it at a rate between them");
    }
}

TEST_CASE ("WAV headers: a file the header reader can't make sense of is left to the decoder")
{
    // An RF64 file (64-bit sizes), and a data chunk whose size was never filled in.
    CHECK_FALSE (kits::readWavHeader (fixture ("rf64-pcm16-mono.wav")));

    const auto unknownSize = kits::readWavHeader (fixture ("data-size-unknown.wav"));
    REQUIRE (unknownSize);
    CHECK_FALSE (unknownSize->frames);
    CHECK_FALSE (kits::sampleFormatProblem (*unknownSize)); // so it isn't refused for being too long

    // Not a WAV at all, cut short, or a chunk that runs past the end.
    CHECK_FALSE (readBytes ("not a wave file", 15));
    CHECK_FALSE (readBytes ("RIFF", 4));
    const auto header = pcmHeader (1, 44100, 16, 64);
    CHECK_FALSE (readBytes (header.substr (0, 30), 30));

    // No "fmt " chunk: it's been renamed.
    auto noFormat = header;
    noFormat.replace (12, 4, "LIST");
    CHECK_FALSE (readBytes (noFormat, noFormat.size() + 64));

    // A chunk before the format that claims more bytes than the file has left.
    auto runsPast = header;
    runsPast.replace (12, 4, "JUNK");
    runsPast.replace (16, 4, std::string ("\xff\xff\x00\x00", 4));
    CHECK_FALSE (readBytes (runsPast, runsPast.size() + 64));

    CHECK_FALSE (kits::readWavHeader (fixture ("no-such-file.wav")));
}
