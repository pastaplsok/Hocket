#pragma once

// The rules a valid project keeps (SPEC §4.1, §4.4, §5.2), shared by the
// project reader and the command bus, so that the two can't drift apart and
// every project the bus makes reads back.
//
// Each check returns what's wrong, written for an LLM reader, or nullopt when
// nothing is. Callers put where it is in front: "track 'trk-bass': " in the
// reader, the command in the bus.

#include "core/notes/NotesSyntax.h"
#include "core/project/Project.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::project
{
    inline constexpr std::size_t maxIdLength = 64;         // bytes, the prefix included (SPEC §4.1)
    inline constexpr std::size_t maxTrackNameLength = 100; // bytes
    inline constexpr std::size_t maxKitNameLength = 64;    // bytes
    inline constexpr double minTempo = 20.0;               // quarter notes per minute
    inline constexpr double maxTempo = 999.0;

    // Lowercase letters and digits in words joined by single hyphens: "bass", "bass-verse-2".
    bool isHyphenatedName (std::string_view name);

    // Valid UTF-8 with no control characters (so no line breaks or tabs), 1 to
    // maxBytes bytes long: text that a list can show on one line.
    bool isOneLineText (std::string_view text, std::size_t maxBytes);

    //==============================================================================
    enum class IdKind
    {
        track,
        clip,
        device
    };

    // "trk-", "clp-", "dev-"
    std::string_view idPrefix (IdKind kind);

    // An ID is its kind's prefix, then lowercase letters and digits in words
    // joined by single hyphens, at most maxIdLength bytes in all (SPEC §4.1).
    std::optional<std::string> idProblem (std::string_view id, IdKind kind);

    // The ASCII letters and digits of text, lowercased, in words joined by
    // single hyphens: "Bass (Verse) #2" is "bass-verse-2". Empty if it has none.
    std::string hyphenatedWords (std::string_view text);

    // A new ID made of words (hyphenated, as hyphenatedWords makes them) that
    // nothing in the project has (SPEC §4.1): the prefix and the words, then
    // with "-2", "-3" and so on, starting from firstNumber (1: the words
    // alone first). The words are cut short to fit maxIdLength.
    std::string unusedId (const Project& project, IdKind kind, std::string_view words, int firstNumber = 1);

    //==============================================================================
    // One line of text, 1 to maxTrackNameLength bytes (isOneLineText).
    std::optional<std::string> trackNameProblem (std::string_view name);

    // minTempo to maxTempo quarter notes per minute.
    std::optional<std::string> tempoProblem (double bpm);

    // For messages: "120", "97.5".
    std::string formatTempo (double bpm);

    // "midi" or "drums".
    notes::Parsed<TrackType> parseTrackType (std::string_view text);

    //==============================================================================
    // "hocket:synth" or "hocket:drum-sampler".
    notes::Parsed<DeviceType> parseDeviceType (std::string_view text);

    // A drum sampler goes on a drums track only. Says so, and what kind of
    // track this one is; the caller says how to fix it.
    std::optional<std::string> devicePlacementProblem (DeviceType device, TrackType track);

    // A kit is a folder, assets/kits/<name> (SPEC §8.4): a hyphenated name
    // (isHyphenatedName), at most maxKitNameLength bytes.
    std::optional<std::string> kitNameProblem (std::string_view kit);

    //==============================================================================
    struct DrumMapEntry
    {
        std::string name;
        int note = 0; // MIDI note number; anything outside 0 to 127 is reported as out of range
    };

    // Why an entry can't go in the map: its name came earlier or isn't a valid
    // drum name (notes::drumNameProblem), its note isn't 0 to 127, or an
    // earlier entry has the note.
    struct DrumMapProblem
    {
        std::size_t entry = 0;  // index into the entries
        std::string message;    // starts with the name in double quotes, or says what's wrong with it
        bool aboutNote = false; // the note is what's wrong, not the name
    };

    // Adds the entries to map in order, skipping the ones that can't go in,
    // and says why each of those can't.
    std::vector<DrumMapProblem> buildDrumMap (std::span<const DrumMapEntry> entries, notes::DrumMap& map);

    //==============================================================================
    // "clips/<name>.notes", with a hyphenated name (isHyphenatedName).
    std::optional<std::string> clipFileProblem (std::string_view file);

    // A song is at most maxSongLengthInBars long, so a clip from start to end
    // must end by the end of that bar. Suggests where it could start instead.
    std::optional<std::string> songLengthProblem (notes::Meter meter, notes::Ticks start, notes::Ticks end);
}
