#pragma once

// Field-level syntax of the .notes format (SPEC §5): parsing and canonical
// formatting of positions, pitches, lengths and velocities.

#include "core/notes/Notes.h"

#include <optional>
#include <string>
#include <string_view>

namespace hocket::notes
{
    template <typename T>
    struct Parsed
    {
        std::optional<T> value;
        std::string error; // when there's no value: what's wrong, and what valid input looks like
    };

    struct SpelledPitch
    {
        int pitch = 0;
        Accidental spelling = Accidental::natural;

        bool operator== (const SpelledPitch&) const = default;
    };

    // bar.beat[.sixteenth][+ticks], 1-based, e.g. "1.1", "2.3.2", "1.1+30".
    // Beats are the time signature's (eighths in 6/8); the third number counts
    // sixteenths within the beat, and +ticks are 960 per quarter note.
    Parsed<Ticks> parsePosition (std::string_view text, Meter meter);
    std::string formatPosition (Ticks position, Meter meter);

    // "4/4", "6/8": the top number 1 to Meter::maxNumerator, the bottom one 2, 4, 8 or 16.
    // Written with Meter::text().
    Parsed<Meter> parseMeter (std::string_view text);

    // Uppercase letter, optional # or b, octave: "C2", "F#4", "Bb3", "C-1"
    Parsed<SpelledPitch> parsePitch (std::string_view text);
    std::string formatPitch (int pitch, Accidental spelling);

    // Fraction of a whole note ("1/8"), triplet ("1/8t") or ticks ("250tk")
    Parsed<Ticks> parseLength (std::string_view text);
    std::string formatLength (Ticks length);

    // "v1" to "v127"
    Parsed<int> parseVelocity (std::string_view text);
    std::string formatVelocity (int velocity);

    bool isBlackKey (int pitch);

    // "cc74", "pb", "at" (channel aftertouch): a controller line's second field.
    bool looksLikeController (std::string_view text);

    // A note name written with a lowercase letter: "c2", "eb3", "f#4", "b-1".
    bool looksLikeLowercaseNoteName (std::string_view text);
}
