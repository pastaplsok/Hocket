#pragma once

// The .notes clip format (SPEC §5): the in-memory model, the parser, and the
// canonical writer.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::notes
{
    // Time inside a clip, counted from the clip start.
    using Ticks = std::int64_t;
    inline constexpr Ticks ticksPerBeat = 960;

    // Milestone 0 supports n/4 meters only, so a beat is always a quarter note.
    struct Meter
    {
        int beatsPerBar = 4;

        Ticks ticksPerBar() const { return beatsPerBar * ticksPerBeat; }
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
        Ticks length = ticksPerBeat;
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
    ParseResult parse (std::string_view text, Meter meter = {});

    // Writes a clip in canonical form (SPEC §5.4). The clip must be valid:
    // pitches 0-127, velocities 1-127, lengths > 0, starts inside the clip.
    std::string write (const Clip& clip, Meter meter = {});

    // "<sourceName>:<line>:<column>: <message>"
    std::string formatDiagnostic (const Diagnostic& diagnostic, std::string_view sourceName);
}
