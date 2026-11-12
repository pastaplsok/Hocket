// Values with units, and pan positions (SPEC §4.4).

#include "core/units/Units.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>

using namespace hocket::units;

namespace
{
    double volumeOf (std::string_view text)
    {
        const auto parsed = parse (text, volume());
        INFO (text << ": " << parsed.error);
        REQUIRE (parsed.value);
        return *parsed.value;
    }

    std::string volumeError (std::string_view text)
    {
        const auto parsed = parse (text, volume());
        CHECK_FALSE (parsed.value);
        return parsed.error;
    }

    // As a device parameter will be in Milestone 4: more than one unit.
    constexpr Unit frequencyUnits[] = { { "Hz", 1.0 }, { "kHz", 1000.0 } };
    const Quantity cutoff { "cutoff", frequencyUnits, 20.0, 20000.0, 0, false, false, "'2400Hz', '2.4kHz'" };
}

TEST_CASE ("volumes are decibels, rounded to 0.1, from -96dB to +6dB, or -infdB")
{
    CHECK (volumeOf ("0dB") == 0.0);
    CHECK (volumeOf ("-6dB") == -6.0);
    CHECK (volumeOf ("+3dB") == 3.0);
    CHECK (volumeOf ("3dB") == 3.0);
    CHECK (volumeOf ("-6.5dB") == -6.5);
    CHECK (volumeOf ("-6.04dB") == -6.0);
    CHECK (volumeOf ("-6.06 dB") == volumeOf ("-6.1dB")); // a space before the unit is fine
    CHECK (volumeOf ("-0.04dB") == 0.0);
    CHECK_FALSE (std::signbit (volumeOf ("-0.04dB")));   // never -0
    CHECK (volumeOf ("-96dB") == -96.0);
    CHECK (volumeOf ("+6dB") == 6.0);
    CHECK (volumeOf ("6.04dB") == 6.0);                   // rounds into range
    CHECK (std::isinf (volumeOf ("-infdB")));
    CHECK (volumeOf ("-inf dB") < 0.0);
}

TEST_CASE ("volumes are written in one way")
{
    const auto written = [] (double value) { return format (value, volume()); };

    CHECK (written (0.0) == "0dB");
    CHECK (written (-6.0) == "-6dB");
    CHECK (written (-6.5) == "-6.5dB");
    CHECK (written (3.0) == "+3dB");
    CHECK (written (0.1) == "+0.1dB");
    CHECK (written (-96.0) == "-96dB");
    CHECK (written (-std::numeric_limits<double>::infinity()) == "-infdB");

    // Reading back what was written gives the same value.
    std::mt19937 rng (20261112);
    std::uniform_int_distribution<int> tenths (-960, 60);

    for (int i = 0; i < 2000; ++i)
    {
        const auto value = tenths (rng) / 10.0;
        const auto parsed = parse (format (value, volume()), volume());
        REQUIRE (parsed.value);
        CHECK (*parsed.value == volumeOf (format (*parsed.value, volume())));
        CHECK (format (*parsed.value, volume()) == format (value, volume()));
    }
}

TEST_CASE ("a wrong volume says what's wrong and how it's written")
{
    const std::string how = ". Write a number and a unit, e.g. '-6dB', '0dB' (unchanged), '+3dB', '-infdB' (silence)";

    CHECK (volumeError ("") == "'' isn't a valid volume: it's empty" + how);
    CHECK (volumeError ("-6") == "'-6' isn't a valid volume: it has no unit; volume is written in dB" + how);
    CHECK (volumeError ("-6db") == "'-6db' isn't a valid volume: units are case-sensitive: write 'dB', not 'db'" + how);
    CHECK (volumeError ("-6 decibels") == "'-6 decibels' isn't a valid volume: 'decibels' isn't a unit volume is written in; use dB" + how);
    CHECK (volumeError ("dB") == "'dB' isn't a valid volume: it needs a number first" + how);
    CHECK (volumeError ("-.5dB") == "'-.5dB' isn't a valid volume: it needs a number first" + how);
    CHECK (volumeError ("-6.dB") == "'-6.dB' isn't a valid volume: a '.' needs digits after it" + how);
    CHECK (volumeError ("1e3dB") == "'1e3dB' isn't a valid volume: 'e3dB' isn't a unit volume is written in; use dB" + how);
    CHECK (volumeError ("+infdB") == "'+infdB' isn't a valid volume: only '-inf' is allowed, with the minus sign" + how);
    CHECK (volumeError ("+12dB") == "'+12dB' is out of range: volume goes from -96dB to +6dB, or -infdB, "
                                    "e.g. '-6dB', '0dB' (unchanged), '+3dB', '-infdB' (silence)");
    CHECK (volumeError ("-100dB").starts_with ("'-100dB' is out of range"));
    CHECK (volumeError ("6.06dB").starts_with ("'6.06dB' is out of range")); // 6.1 after rounding
    CHECK (volumeError (std::string (400, '9') + "dB").ends_with ("isn't a valid volume: the number is too large" + how));
}

TEST_CASE ("a quantity can have several units, and the writer uses the first")
{
    CHECK (parse ("2.4kHz", cutoff).value == 2400.0);
    CHECK (parse ("2400Hz", cutoff).value == 2400.0);
    CHECK (parse ("440.4Hz", cutoff).value == 440.0);
    CHECK (format (2400.0, cutoff) == "2400Hz");
    CHECK (parse ("2.4KHZ", cutoff).error.starts_with ("'2.4KHZ' isn't a valid cutoff: units are case-sensitive: write 'kHz', not 'KHZ'"));
    CHECK (parse ("2.4MHz", cutoff).error.starts_with ("'2.4MHz' isn't a valid cutoff: 'MHz' isn't a unit cutoff is written in; use Hz or kHz"));
    CHECK (parse ("-infHz", cutoff).error.starts_with ("'-infHz' isn't a valid cutoff: it needs a number, not 'inf'"));
    CHECK (parse ("25kHz", cutoff).error == "'25kHz' is out of range: cutoff goes from 20Hz to 20000Hz, e.g. '2400Hz', '2.4kHz'");
}

TEST_CASE ("pan is C, L1 to L100, or R1 to R100")
{
    CHECK (parsePan ("C").value == 0);
    CHECK (parsePan ("L1").value == -1);
    CHECK (parsePan ("L100").value == -100);
    CHECK (parsePan ("R20").value == 20);
    CHECK (parsePan ("R100").value == 100);
    CHECK (parsePan ("R020").value == 20);

    for (int pan = -maxPan; pan <= maxPan; ++pan)
        CHECK (parsePan (formatPan (pan)).value == pan);

    CHECK (formatPan (0) == "C");
    CHECK (formatPan (-20) == "L20");
    CHECK (formatPan (100) == "R100");
}

TEST_CASE ("a wrong pan says what's wrong and how it's written")
{
    const auto error = [] (std::string_view text)
    {
        const auto parsed = parsePan (text);
        CHECK_FALSE (parsed.value);
        return parsed.error;
    };

    const std::string how = ". Pan is 'C' (centre), 'L1' to 'L100' (left) or 'R1' to 'R100' (right), e.g. 'L20'";

    CHECK (error ("") == "'' isn't a pan position: it's empty" + how);
    CHECK (error ("l20") == "'l20' isn't a pan position: write it in capitals: 'L20'" + how);
    CHECK (error ("c") == "'c' isn't a pan position: write it in capitals: 'C'" + how);
    CHECK (error ("L0") == "'L0' isn't a pan position: the centre is written 'C'" + how);
    CHECK (error ("C0") == "'C0' isn't a pan position: the centre is just 'C'" + how);
    CHECK (error ("L101") == "'L101' isn't a pan position: hard left is L100" + how);
    CHECK (error ("R1000") == "'R1000' isn't a pan position: R needs a whole number from 1 to 100 after it" + how);
    CHECK (error ("L") == "'L' isn't a pan position: L needs a whole number from 1 to 100 after it" + how);
    CHECK (error ("L-20") == "'L-20' isn't a pan position: L needs a whole number from 1 to 100 after it" + how);
    CHECK (error ("-20") == "'-20' isn't a pan position: it must be C, or a capital L or R and a number" + how);
    CHECK (error ("centre") == "'centre' isn't a pan position: it must be C, or a capital L or R and a number" + how);
    CHECK (error ("right") == "'right' isn't a pan position: it must be C, or a capital L or R and a number" + how);
}
