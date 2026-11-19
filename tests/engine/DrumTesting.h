#pragma once

// Helpers for the drum sampler's engine tests (DrumSamplerTests.cpp and the
// stress tests): made-up kits written as 32-bit float samples in a non-ASCII
// temp folder, so every output sample can be worked out exactly, and ways to
// compare the output with what it should be.

#include "EngineTesting.h"
#include "common/TemporaryFolder.h"
#include "common/WavWriter.h"
#include "core/Paths.h"
#include "core/kits/KitCatalog.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace hocket::testing
{
    // A decaying tone, never exactly zero at its start, so a hit's first
    // sample shows where it starts. Each piece gets its own, so they can't be
    // mistaken for each other.
    inline std::vector<float> tone (int length, double step, float level)
    {
        std::vector<float> samples (static_cast<std::size_t> (length));

        for (int i = 0; i < length; ++i)
            samples[static_cast<std::size_t> (i)] = level * static_cast<float> (std::sin (step * (i + 1)) * (1.0 - 0.9 * i / length));

        return samples;
    }

    struct TestPiece
    {
        std::string name;
        int note = 0;
        std::vector<std::vector<float>> channels; // one or two
        std::string level = "0dB";
    };

    // A user kit in a folder of its own: kit.json, and each piece's sample as
    // <name>.wav, 32-bit float, so it decodes to exactly these values.
    inline void writeKit (const std::filesystem::path& folder, const std::vector<TestPiece>& pieces, double rate,
                          const std::vector<std::vector<std::string>>& chokeGroups = {})
    {
        std::filesystem::create_directories (folder);
        std::ostringstream json;
        json << "{\n  \"format\": \"hocket-kit\",\n  \"formatVersion\": 0,\n  \"description\": \"Made up\",\n  \"pieces\": [\n";

        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            const auto& piece = pieces[i];
            json << "    { \"name\": \"" << piece.name << "\", \"note\": " << piece.note << ", \"file\": \"" << piece.name << ".wav\", \"level\": \""
                 << piece.level << "\" }" << (i + 1 < pieces.size() ? ",\n" : "\n");
            writeWav (folder / pathFromUtf8 (piece.name + ".wav"), piece.channels, rate, WavEncoding::float32);
        }

        json << "  ],\n  \"chokeGroups\": [";

        for (std::size_t g = 0; g < chokeGroups.size(); ++g)
        {
            json << (g > 0 ? ", [" : "[");

            for (std::size_t i = 0; i < chokeGroups[g].size(); ++i)
                json << (i > 0 ? ", \"" : "\"") << chokeGroups[g][i] << "\"";

            json << "]";
        }

        json << "]\n}\n";
        std::ofstream (folder / pathFromUtf8 ("kit.json"), std::ios::binary) << json.str();
    }

    // The user's kit folder, a catalog of it and a library over that. Write
    // the kits first; declare it before the EditSync, which it must outlive.
    struct Kits
    {
        TemporaryFolder temp { unicodeName };
        std::filesystem::path userFolder = temp.path / pathFromUtf8 ("Kits");
        std::optional<kits::KitCatalog> catalog;
        std::optional<engine::KitLibrary> library;

        void write (std::string_view name, const std::vector<TestPiece>& pieces, double rate = 44100.0,
                    const std::vector<std::vector<std::string>>& chokeGroups = {})
        {
            writeKit (userFolder / pathFromUtf8 (name), pieces, rate, chokeGroups);
        }

        engine::KitLibrary& open()
        {
            catalog.emplace (std::filesystem::path(), userFolder);
            library.emplace (*catalog);
            return *library;
        }
    };

    // trk-drums with Hocket's drum sampler playing `kit`, and the standard
    // map. Clips are written with note names (C2 for the kick, 36), which
    // parse without the map.
    inline TestTrack drumsTrack (std::string kit, std::vector<TestClip> clips)
    {
        project::Track track { "trk-drums", "Drums", project::TrackType::drums, { { "dev-drums-kit", project::DeviceType::drumSampler, std::move (kit) } }, {} };
        track.drumMap = notes::standardDrumMap();
        return { std::move (track), std::move (clips) };
    }

    // The drum sampler on the Edit's first track.
    inline engine::DrumSamplerPlugin& samplerOf (engine::EditSync& sync)
    {
        const auto samplers = tracktion::getAudioTracks (sync.edit())[0]->pluginList.getPluginsOfType<engine::DrumSamplerPlugin>();
        REQUIRE (samplers.size() == 1);
        return *samplers.getFirst();
    }

    // A piece's sample value at `i`, or 0 outside it.
    inline float at (const std::vector<float>& samples, std::int64_t i)
    {
        return i >= 0 && i < static_cast<std::int64_t> (samples.size()) ? samples[static_cast<std::size_t> (i)] : 0.0f;
    }

    // Compares a channel from `from` to `to` with what it should be, within
    // `tolerance`; empty if it matches, or the first mismatch and how many.
    inline std::string mismatches (const juce::AudioBuffer<float>& out, int channel, int from, int to, const std::function<float (int)>& expected,
                                   float tolerance = 0.0f)
    {
        int count = 0;
        std::ostringstream first;
        first.precision (9);

        for (int i = from; i < to; ++i)
        {
            const auto want = expected (i);
            const auto got = out.getSample (channel, i);

            if (std::abs (got - want) > tolerance && count++ == 0)
                first << "channel " << channel << ", sample " << i << ": " << got << ", should be " << want;
        }

        return count == 0 ? std::string() : first.str() + " (" + std::to_string (count) + " samples off)";
    }
}
