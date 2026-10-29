#include "core/notes/NotesSyntax.h"

#include <cstdint>
#include <numeric>

namespace hocket::notes
{
namespace
{
    constexpr Ticks ticksPerWholeNote = 4 * ticksPerBeat;
    constexpr Ticks ticksPerSixteenth = ticksPerBeat / 4;
    constexpr Ticks maxCanonicalDenominator = 64;

    // A run of 1-9 decimal digits; anything else (signs, spaces, overflow) is rejected.
    std::optional<std::int64_t> parseDigits (std::string_view text)
    {
        if (text.empty() || text.size() > 9)
            return {};

        std::int64_t value = 0;

        for (const char c : text)
        {
            if (c < '0' || c > '9')
                return {};

            value = value * 10 + (c - '0');
        }

        return value;
    }

    std::string quoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    bool isPowerOfTwo (Ticks n)
    {
        return n > 0 && (n & (n - 1)) == 0;
    }

    // "num/den" if ticks is a whole-note fraction with a small power-of-two denominator.
    std::optional<std::string> formatAsFraction (Ticks ticks)
    {
        const auto divisor = std::gcd (ticks, ticksPerWholeNote);
        const auto numerator = ticks / divisor;
        const auto denominator = ticksPerWholeNote / divisor;

        if (! isPowerOfTwo (denominator) || denominator > maxCanonicalDenominator)
            return {};

        return std::to_string (numerator) + "/" + std::to_string (denominator);
    }
}

//==============================================================================
Parsed<Ticks> parsePosition (std::string_view text, Meter meter)
{
    const auto fail = [text] (const std::string& why)
    {
        return Parsed<Ticks> { {}, quoted (text) + " isn't a valid position: " + why
                                       + ". Positions are bar.beat[.sixteenth][+ticks], e.g. '1.1', '2.3.2', '1.1+30'" };
    };

    auto barBeat = text;
    Ticks extraTicks = 0;

    if (const auto plus = text.find ('+'); plus != std::string_view::npos)
    {
        const auto ticks = parseDigits (text.substr (plus + 1));

        if (! ticks)
            return fail ("the part after '+' must be a whole number of ticks");

        extraTicks = *ticks;
        barBeat = text.substr (0, plus);
    }

    std::int64_t parts[3] {};
    int count = 0;

    for (std::size_t start = 0;;)
    {
        const auto dot = barBeat.find ('.', start);
        const auto piece = barBeat.substr (start, dot == std::string_view::npos ? std::string_view::npos : dot - start);

        if (count == 3)
            return fail ("it has more than three numbers separated by dots");

        const auto number = parseDigits (piece);

        if (! number)
            return fail (piece.empty() ? std::string ("a number is missing") : quoted (piece) + " isn't a whole number");

        parts[count++] = *number;

        if (dot == std::string_view::npos)
            break;

        start = dot + 1;
    }

    if (count < 2)
        return fail ("it needs at least a bar and a beat");

    const auto bar = parts[0];
    const auto beat = parts[1];
    const auto sixteenth = count == 3 ? parts[2] : 1;

    if (bar < 1)
        return fail ("bars start at 1");

    if (beat < 1 || beat > meter.beatsPerBar)
        return fail ("beat " + std::to_string (beat) + " doesn't exist in " + std::to_string (meter.beatsPerBar)
                     + "/4 (beats are 1-" + std::to_string (meter.beatsPerBar) + ")");

    if (sixteenth < 1 || sixteenth > 4)
        return fail ("sixteenth " + std::to_string (sixteenth) + " doesn't exist (sixteenths are 1-4)");

    return { (bar - 1) * meter.ticksPerBar() + (beat - 1) * ticksPerBeat + (sixteenth - 1) * ticksPerSixteenth + extraTicks, {} };
}

std::string formatPosition (Ticks position, Meter meter)
{
    const auto bar = position / meter.ticksPerBar() + 1;
    const auto inBar = position % meter.ticksPerBar();
    const auto beat = inBar / ticksPerBeat + 1;
    const auto inBeat = inBar % ticksPerBeat;
    const auto sixteenth = inBeat / ticksPerSixteenth + 1;
    const auto ticks = inBeat % ticksPerSixteenth;

    auto text = std::to_string (bar) + "." + std::to_string (beat);

    if (sixteenth != 1)
        text += "." + std::to_string (sixteenth);

    if (ticks != 0)
        text += "+" + std::to_string (ticks);

    return text;
}

//==============================================================================
Parsed<SpelledPitch> parsePitch (std::string_view text)
{
    const auto fail = [text] (const std::string& why)
    {
        return Parsed<SpelledPitch> { {}, quoted (text) + " isn't a note name: " + why
                                              + ". Note names are an uppercase letter, an optional # or b, and an octave, "
                                                "e.g. 'C2', 'F#4', 'Bb3' (C4 is middle C)" };
    };

    if (text.empty())
        return fail ("it's empty");

    const auto letter = text[0];

    if (letter >= 'a' && letter <= 'g')
        return fail ("the letter must be uppercase");

    if (letter < 'A' || letter > 'G')
        return fail ("it must start with a letter from A to G");

    static constexpr int semitoneOfLetter[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    std::size_t i = 1;
    int accidental = 0;
    auto spelling = Accidental::natural;

    if (i < text.size() && text[i] == '#')
    {
        accidental = 1;
        spelling = Accidental::sharp;
        ++i;
    }
    else if (i < text.size() && text[i] == 'b')
    {
        accidental = -1;
        spelling = Accidental::flat;
        ++i;
    }

    const bool negative = i < text.size() && text[i] == '-';

    if (negative)
        ++i;

    const auto octaveDigits = parseDigits (text.substr (i));

    if (! octaveDigits || *octaveDigits > 9 || (negative && *octaveDigits != 1))
        return fail ("it needs an octave from -1 to 9 after the letter");

    const auto octave = static_cast<int> (negative ? -*octaveDigits : *octaveDigits);
    const auto pitch = (octave + 1) * 12 + semitoneOfLetter[letter - 'A'] + accidental;

    if (pitch < 0)
        return { {}, quoted (text) + " is below the MIDI range; the lowest note is C-1" };

    if (pitch > 127)
        return { {}, quoted (text) + " is above the MIDI range; the highest note is G9" };

    // Cb, Fb, B# and E# land on white keys and are written as the natural note.
    if (! isBlackKey (pitch))
        spelling = Accidental::natural;

    return { SpelledPitch { pitch, spelling }, {} };
}

std::string formatPitch (int pitch, Accidental spelling)
{
    static constexpr const char* sharpNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static constexpr const char* flatNames[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

    const auto pitchClass = pitch % 12;
    const auto octave = pitch / 12 - 1;
    const auto* names = spelling == Accidental::flat ? flatNames : sharpNames;

    return names[pitchClass] + std::to_string (octave);
}

bool isBlackKey (int pitch)
{
    switch (pitch % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

//==============================================================================
Parsed<Ticks> parseLength (std::string_view text)
{
    const auto fail = [text] (const std::string& why)
    {
        return Parsed<Ticks> { {}, quoted (text) + " isn't a valid length: " + why
                                       + ". Lengths are a fraction of a whole note ('1/4', '1/8', '3/16'), "
                                         "a triplet ('1/8t'), or ticks ('250tk', 960 per beat)" };
    };

    if (text.ends_with ("tk"))
    {
        const auto ticks = parseDigits (text.substr (0, text.size() - 2));

        if (! ticks)
            return fail ("the part before 'tk' must be a whole number");

        if (*ticks == 0)
            return fail ("it must be longer than zero");

        return { *ticks, {} };
    }

    const bool triplet = text.ends_with ('t');
    const auto fraction = triplet ? text.substr (0, text.size() - 1) : text;
    const auto slash = fraction.find ('/');

    if (slash == std::string_view::npos)
        return fail ("it needs a '/' between two numbers, or 'tk' after a number of ticks");

    const auto numerator = parseDigits (fraction.substr (0, slash));
    const auto denominator = parseDigits (fraction.substr (slash + 1));

    if (! numerator || ! denominator)
        return fail ("both sides of the '/' must be whole numbers");

    if (*numerator == 0)
        return fail ("it must be longer than zero");

    if (*denominator == 0)
        return fail ("the bottom number can't be zero");

    // A triplet is two thirds of the plain length.
    const auto scaledNumerator = ticksPerWholeNote * *numerator * (triplet ? 2 : 1);
    const auto scaledDenominator = *denominator * (triplet ? 3 : 1);

    if (scaledNumerator % scaledDenominator != 0)
        return fail ("it doesn't come out to a whole number of ticks (960 per beat)");

    return { scaledNumerator / scaledDenominator, {} };
}

std::string formatLength (Ticks length)
{
    if (auto fraction = formatAsFraction (length))
        return *fraction;

    if ((length * 3) % 2 == 0)
        if (auto fraction = formatAsFraction (length * 3 / 2))
            return *fraction + "t";

    return std::to_string (length) + "tk";
}

//==============================================================================
Parsed<int> parseVelocity (std::string_view text)
{
    const auto number = text.starts_with ('v') ? parseDigits (text.substr (1)) : std::nullopt;

    if (! number)
        return { {}, quoted (text) + " isn't a velocity: velocities are 'v' and a number from 1 to 127, e.g. 'v96'" };

    if (*number == 0)
        return { {}, quoted (text) + " isn't a valid velocity: the lowest is v1 (velocities are v1-v127)" };

    if (*number > 127)
        return { {}, quoted (text) + " isn't a valid velocity: the highest is v127 (velocities are v1-v127)" };

    return { static_cast<int> (*number), {} };
}

std::string formatVelocity (int velocity)
{
    return "v" + std::to_string (velocity);
}
}
