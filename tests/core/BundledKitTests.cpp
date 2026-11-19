// The kits bundled with Hocket (SPEC §8.4), as checked out in assets/kits:
// each reads cleanly, uses the standard names on the standard notes (SPEC
// §5.2), and has 16-bit samples (Lee's rule for bundled kits). The
// electronic kit has the pieces Lee asked for, and its samples are 44.1 kHz
// and mono (Lee, 2026-11-18). Its headroom is checked by rendering it, in
// tests/engine.

#include "core/Paths.h"
#include "core/kits/KitCatalog.h"
#include "core/kits/WavHeader.h"
#include "core/notes/DrumMap.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace hocket;

namespace
{
    kits::KitCatalog bundledKits()
    {
        return { pathFromUtf8 (HOCKET_KITS_DIR), {} };
    }
}

TEST_CASE ("bundled kits: every one reads cleanly, and the electronic kit is among them")
{
    auto catalog = bundledKits();

    for (const auto& problem : catalog.problems())
        MESSAGE (problem);

    CHECK (catalog.problems().empty());
    const auto found = catalog.find ("electronic");
    REQUIRE_MESSAGE (found.kit != nullptr, found.problem);
    CHECK (found.kit->source == kits::Source::bundled);
}

TEST_CASE ("bundled kits use the standard names on the standard notes, and may add pieces on other notes")
{
    auto catalog = bundledKits();
    const auto& standard = notes::standardDrumMap();
    REQUIRE_FALSE (catalog.kits().empty());

    for (const auto* kit : catalog.kits())
    {
        CAPTURE (kit->name);

        for (const auto& piece : kit->manifest.pieces)
        {
            CAPTURE (piece.name);

            if (const auto* name = standard.nameOf (piece.note))
                CHECK (*name == piece.name); // a standard note has its standard name

            if (const auto note = standard.noteOf (piece.name))
                CHECK (*note == piece.note); // a standard name is on its own note
        }
    }
}

TEST_CASE ("bundled kits' samples are 16-bit, and the electronic kit's are 44.1 kHz and mono")
{
    auto catalog = bundledKits();

    for (const auto* kit : catalog.kits())
    {
        CAPTURE (kit->name);

        for (const auto& piece : kit->manifest.pieces)
        {
            CAPTURE (piece.file);
            const auto header = kits::readWavHeader (kit->folder / pathFromUtf8 (piece.file));
            REQUIRE (header.has_value());
            CHECK (header->encoding == kits::WavHeader::Encoding::pcm);
            CHECK (header->bitsPerSample == 16);
            CHECK_FALSE (kits::sampleFormatProblem (*header).has_value());

            if (kit->name == "electronic")
            {
                CHECK (header->sampleRate == 44100.0);
                CHECK (header->channels == 1);
            }
        }
    }
}

TEST_CASE ("the electronic kit has the classic drum-machine set Lee asked for, with the hats in one choke group and each kick in its own")
{
    auto catalog = bundledKits();
    const auto* kit = catalog.find ("electronic").kit;
    REQUIRE (kit != nullptr);
    const auto& manifest = kit->manifest;

    for (const auto* name : { "kick", "kick2", "snare", "clap", "sidestick", "hat-closed", "hat-open", "hat-pedal", "tom-floor-low",
                              "tom-floor-high", "tom-low", "tom-mid-low", "tom-mid-high", "tom-high", "crash", "cowbell", "claves" })
    {
        CAPTURE (std::string (name));
        CHECK (manifest.pieceNamed (name) != nullptr);
    }

    const auto hats = manifest.chokeGroupOf ("hat-closed");
    CHECK (hats >= 0);
    CHECK (manifest.chokeGroupOf ("hat-open") == hats);
    CHECK (manifest.chokeGroupOf ("hat-pedal") == hats);

    for (const auto* kick : { "kick", "kick2" })
    {
        const auto group = manifest.chokeGroupOf (kick);
        REQUIRE (group >= 0);
        CHECK (manifest.chokeGroups[static_cast<std::size_t> (group)] == std::vector<std::string> { kick });
    }
}
