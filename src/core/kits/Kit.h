#pragma once

// A drum kit's manifest (SPEC §8.4): kit.json, in the kit's folder beside
// its samples. It says which piece each note plays, from which sample, at
// what level, and which pieces cut each other off (choke groups, SPEC
// §5.2). Read strictly, like project.json, so a project's drum map, the
// bus and the overview know a kit's pieces without loading any audio.

#include "core/project/Project.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::kits
{
    // Version 0 is the pre-release format, as project.json's is (SPEC §4.1).
    inline constexpr int formatVersion = 0;
    inline constexpr std::string_view manifestFileName = "kit.json";
    inline constexpr std::size_t maxDescriptionLength = 200; // bytes
    inline constexpr std::size_t maxSampleFileLength = 200;  // bytes

    struct Piece
    {
        std::string name;   // as a drum map names it (notes::drumNameProblem)
        int note = 0;       // MIDI note number, 0 to 127
        std::string file;   // the sample, relative to the kit's folder, with forward slashes
        double level = 0.0; // dB (units::volume()), never -inf; a hit at v127 plays at this level

        bool operator== (const Piece&) const = default;
    };

    struct Manifest
    {
        std::string description;                           // one line, e.g. "Electronic, 808-style"
        std::vector<Piece> pieces;                         // as written; one note per piece, and one piece per note
        std::vector<std::vector<std::string>> chokeGroups; // piece names; each piece is in one group at most

        const Piece* pieceOn (int note) const;
        const Piece* pieceNamed (std::string_view name) const;

        // The index of the piece's choke group in chokeGroups, or -1 for none.
        int chokeGroupOf (std::string_view piece) const;

        bool operator== (const Manifest&) const = default;
    };

    struct ManifestResult
    {
        std::optional<Manifest> manifest;        // set only when there are no errors
        std::vector<project::Diagnostic> errors; // in kit.json, by place
    };

    ManifestResult parseManifest (std::string_view text);

    // A sample's file, relative to the kit's folder: a .wav file, with forward
    // slashes, inside the folder (no absolute paths, "." or ".."), and no
    // characters Windows can't put in a file name.
    std::optional<std::string> sampleFileProblem (std::string_view file);
}
