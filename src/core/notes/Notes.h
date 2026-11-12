#pragma once

// The .notes clip format (SPEC §5): the in-memory model, the parser, and the
// canonical writer.

#include "core/notes/DrumMap.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::notes
{
    // Time inside a clip, counted from the clip start: 960 ticks per quarter
    // note, whatever the time signature (SPEC §5.1).
    using Ticks = std::int64_t;
    inline constexpr Ticks ticksPerQuarterNote = 960;
    inline constexpr Ticks ticksPerWholeNote = 4 * ticksPerQuarterNote;
    inline constexpr Ticks ticksPerSixteenth = ticksPerQuarterNote / 4;

    // The longest a clip can be (SPEC §5.4), so a typo in a bar number can't
    // make a clip, and a loop, thousands of bars long.
    inline constexpr int maxLengthInBars = 999;

    // The time signature. A beat is the denominator's note: a quarter in 4/4,
    // an eighth in 6/8 (SPEC §4.4). Tempo is in quarter notes per minute
    // whatever the signature.
    struct Meter
    {
        static constexpr int maxNumerator = 32;

        int numerator = 4;   // beats in a bar, 1 to maxNumerator
        int denominator = 4; // the beat's note: 2, 4, 8 or 16

        Ticks ticksPerBeat() const { return ticksPerWholeNote / denominator; }
        Ticks ticksPerBar() const { return numerator * ticksPerBeat(); }
        int sixteenthsPerBeat() const { return 16 / denominator; }

        // "6/8"
        std::string text() const { return std::to_string (numerator) + "/" + std::to_string (denominator); }

        bool operator== (const Meter&) const = default;
    };

    // How a black key is written: Eb2 and D#2 are the same pitch.
    enum class Accidental
    {
        natural,
        sharp,
        flat
    };

    // Comment text is everything after the '#', kept exactly (minus trailing whitespace).
    struct Comments
    {
        std::vector<std::string> leading;     // full-line comments directly above the line
        std::optional<std::string> trailing;  // comment at the end of the line

        bool operator== (const Comments&) const = default;
    };

    struct Note
    {
        Ticks start = 0;
        Ticks length = ticksPerQuarterNote;
        int pitch = 60;     // MIDI note number, 0-127 (C4 = 60)
        int velocity = 100; // 1-127
        Accidental spelling = Accidental::natural;
        Comments comments;

        bool operator== (const Note&) const = default;
    };

    struct Clip
    {
        int lengthInBars = 1;
        std::vector<std::string> header;    // comments at the top of the file
        Comments lengthComments;            // comments on the @length line
        std::vector<Note> notes;            // in file order; the writer sorts them
        std::vector<std::string> footer;    // comments after the last line

        bool operator== (const Clip&) const = default;
    };

    struct Diagnostic
    {
        int line = 0;   // 1-based
        int column = 0; // 1-based, in bytes
        std::string message;

        bool operator== (const Diagnostic&) const = default;
    };

    struct ParseResult
    {
        std::optional<Clip> clip;       // set only when there are no errors
        std::vector<Diagnostic> errors; // in line order
    };

    // Parses .notes text. Every error is reported, not just the first (up to a limit).
    //
    // A clip on a drums track is read with the track's drum map (SPEC §5.2):
    // its names stand for their notes, and notes it names are kept with the
    // spelling namedSpelling() gives, since the writer writes their names.
    // Without a map, a drum name is an error saying the track has none.
    ParseResult parse (std::string_view text, Meter meter = {}, const DrumMap* drumMap = nullptr);

    // Writes a clip in canonical form (SPEC §5.4). The clip must be valid:
    // pitches 0-127, velocities 1-127, lengths > 0, starts inside the clip.
    // With a drum map, a note it names is written by its name; others by note name.
    std::string write (const Clip& clip, Meter meter = {}, const DrumMap* drumMap = nullptr);

    // How a note that a drum map names is spelt in the model: sharp on a black
    // key, so it falls back to "A#5" if the map stops naming it.
    Accidental namedSpelling (int pitch);

    // "<sourceName>:<line>:<column>: <message>"
    std::string formatDiagnostic (const Diagnostic& diagnostic, std::string_view sourceName);
}
