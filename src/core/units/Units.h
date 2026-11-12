#pragma once

// Values written with their units (SPEC §4.1, rule 3): parsing and canonical
// formatting. Track volume uses it now; device parameters ("2.4kHz", "12ms")
// will in Milestone 4. Pan positions ("C", "L20") are here too.

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hocket::units
{
    // A unit a quantity can be written in, and how many of its base unit one
    // is: a kHz is 1000 Hz.
    struct Unit
    {
        std::string_view symbol; // "dB", "Hz", "kHz", "ms", "%"
        double scale = 1.0;
    };

    // What a value means, how it's written, and which values are allowed.
    struct Quantity
    {
        std::string_view name;       // for messages: "volume"
        std::span<const Unit> units; // the first is the base unit, the one the writer uses
        double min = 0.0;            // in the base unit
        double max = 0.0;
        int decimals = 0;            // values are rounded to this many decimal places of the base unit
        bool minusInfinity = false;  // "-inf" with the unit is allowed too, below min ("-infdB": silence)
        bool plusSign = false;       // the writer puts '+' on values above zero ("+3dB")
        std::string_view examples;   // for messages: "'-6dB', '0dB'"
    };

    struct ParsedValue
    {
        std::optional<double> value; // in the base unit, rounded to the quantity's decimals
        std::string error;           // when there's no value: what's wrong, and what valid input looks like
    };

    // A number with an optional sign and decimal point, then a unit, with or
    // without a space between: "-6dB", "+3dB", "-6.5 dB", "2.4kHz".
    ParsedValue parse (std::string_view text, const Quantity& quantity);

    // Canonical form: the base unit, no space, no trailing zeros: "-6dB",
    // "-6.5dB", "+3dB", "0dB", "-infdB". The value must be one parse() could return.
    std::string format (double value, const Quantity& quantity);

    // A track's volume, in dB: -96dB to +6dB in steps of 0.1dB, or -infdB for
    // silence. 0dB leaves the signal as it is (SPEC §4.4).
    const Quantity& volume();

    //==============================================================================
    // Pan: -100 (L100, hard left) to 100 (R100, hard right); 0 is the centre.
    inline constexpr int maxPan = 100;

    struct ParsedPan
    {
        std::optional<int> value;
        std::string error;
    };

    // "C", "L1" to "L100", "R1" to "R100".
    ParsedPan parsePan (std::string_view text);
    std::string formatPan (int pan);
}
