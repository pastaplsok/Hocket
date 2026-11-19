// The headroom rule for bundled kits (Milestone 1's step 7, Lee's decision):
// every piece of a kit struck at once at v127, on a track at 0 dB and C, peaks
// at -1 dBFS or under, choke groups and all. Rendered through Tracktion at
// 44.1 kHz, the electronic kit's own rate; and at 48 kHz played by the
// sampler's own core, the kit converted as a 48 kHz device gets it. (A render
// at 48 kHz would be silent: the sampler asks for the kit at a new rate when
// the render starts, and an offline render doesn't wait.) The track's gain
// stage at 0 dB and C, and the Edit's master, are exactly unity (MixTests.cpp).

#include "DrumTesting.h"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    kits::KitCatalog bundledKits()
    {
        return { pathFromUtf8 (HOCKET_KITS_DIR), {} };
    }

    double dbfs (float peak)
    {
        return 20.0 * std::log10 (static_cast<double> (peak));
    }

    // A clip striking every piece of the kit at 1.1, at v127, by note name.
    std::string everyPieceAtOnce (const kits::Manifest& manifest)
    {
        std::string text = "@length 1bar\n\n";

        for (const auto& piece : manifest.pieces)
            text += "1.1 " + notes::formatPitch (piece.note, notes::Accidental::sharp) + " 1/16 v127\n";

        return text;
    }
}

TEST_CASE ("bundled kits' headroom: every piece struck at once at v127, rendered on a track at 0 dB and C at 44.1 kHz, peaks at -1 dBFS or under")
{
    auto catalog = bundledKits();
    REQUIRE_FALSE (catalog.kits().empty());

    for (const auto* kit : catalog.kits())
    {
        CAPTURE (kit->name);
        engine::KitLibrary library (catalog);
        commands::CommandBus bus (makeProject ({ drumsTrack (kit->name, { makeClip ("clp-all", 0, everyPieceAtOnce (kit->manifest)) }) }));
        engine::EditSync sync (testEngine(), bus, &library);
        library.waitUntilIdle();

        const auto audio = renderLoop (sync);
        REQUIRE (audio.sampleRate == 44100.0);
        const auto peak = audio.buffer.getMagnitude (0, audio.buffer.getNumSamples());
        MESSAGE ("kit " << kit->name << ": every piece at once at v127 peaks at " << dbfs (peak) << " dBFS, rendered at 44.1 kHz");
        CHECK (peak > 0.1f); // it played
        CHECK (dbfs (peak) <= -1.0);
    }
}

TEST_CASE ("render: the electronic kit's audition project plays each piece of its tour where the clip says, and nothing in it clips")
{
    auto catalog = bundledKits();
    engine::KitLibrary library (catalog);
    auto loaded = project::load (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/electronic-kit"));
    REQUIRE (loaded.project);
    commands::CommandBus bus (std::move (*loaded.project));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();

    const auto audio = renderLoop (sync);
    REQUIRE (audio.sampleRate == 44100.0);
    CHECK (audio.buffer.getNumSamples() == audio.sampleAt (62.5)); // 25 bars at 96 BPM

    const auto peak = peakBetween (audio.buffer, 0, audio.buffer.getNumSamples());
    MESSAGE ("the audition project peaks at " << dbfs (peak) << " dBFS");
    CHECK (dbfs (peak) <= -1.0);

    // The tour: a piece every half bar (1.25 s at 96 BPM) from 1.1 to 11.1,
    // the crash at 12.1 and the ride at 13.1. Each comes out of silence, at
    // its place (its attack can take a few ms to reach the threshold).
    std::vector<double> starts;

    for (int piece = 0; piece < 21; ++piece)
        starts.push_back (piece * 1.25);

    starts.push_back (27.5);
    starts.push_back (30.0);

    for (const auto seconds : starts)
    {
        CAPTURE (seconds);
        const auto at = audio.sampleAt (seconds);
        const auto first = firstSoundAt (audio.buffer, at - audio.sampleAt (0.05), 1.0e-4f);
        CHECK (first >= at);
        CHECK (first <= at + audio.sampleAt (0.004));
        CHECK (peakBetween (audio.buffer, at, at + audio.sampleAt (0.2)) > 1.0e-3f);
    }
}

TEST_CASE ("bundled kits' headroom at 48 kHz: converted as a 48 kHz device gets them, every piece struck at once at v127 peaks at -1 dBFS or under")
{
    auto catalog = bundledKits();
    REQUIRE_FALSE (catalog.kits().empty());

    for (const auto* kit : catalog.kits())
    {
        CAPTURE (kit->name);
        engine::KitLibrary library (catalog);
        std::unique_ptr<engine::PlayableKit> playable;
        const auto subscription = library.subscribe (kit->name, 48000.0, [&playable] (std::unique_ptr<engine::PlayableKit> loaded) { playable = std::move (loaded); });
        library.waitUntilIdle();
        REQUIRE (playable != nullptr);
        CHECK (library.counts().conversions > 0); // 44.1 kHz samples, converted

        engine::DrumVoices voices;
        voices.setKit (std::move (playable));
        std::vector<engine::DrumEvent> events;

        for (const auto& piece : kit->manifest.pieces)
            events.push_back ({ engine::DrumEvent::Kind::hit, 0, piece.note, 127 });

        constexpr int length = 3 * 48000;
        std::vector<float> left (length, 0.0f);
        std::vector<float> right (length, 0.0f);
        voices.process (left.data(), right.data(), length, events);

        float peak = 0.0f;

        for (int i = 0; i < length; ++i)
            peak = std::max ({ peak, std::abs (left[static_cast<std::size_t> (i)]), std::abs (right[static_cast<std::size_t> (i)]) });

        MESSAGE ("kit " << kit->name << ": every piece at once at v127 peaks at " << dbfs (peak) << " dBFS, converted to 48 kHz");
        CHECK (peak > 0.1f);
        CHECK (dbfs (peak) <= -1.0);
    }
}
