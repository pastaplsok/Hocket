#include "core/notes/NotesSyntax.h"

#include <doctest/doctest.h>

using namespace hocket::notes;

namespace
{
    constexpr Meter fourFour {};

    Ticks position (std::string_view text)
    {
        const auto parsed = parsePosition (text, fourFour);
        REQUIRE_MESSAGE (parsed.value, parsed.error);
        return *parsed.value;
    }

    Ticks length (std::string_view text)
    {
        const auto parsed = parseLength (text);
        REQUIRE_MESSAGE (parsed.value, parsed.error);
        return *parsed.value;
    }

    SpelledPitch pitch (std::string_view text)
    {
        const auto parsed = parsePitch (text);
        REQUIRE_MESSAGE (parsed.value, parsed.error);
        return *parsed.value;
    }
}

TEST_CASE ("positions parse to ticks from the clip start")
{
    CHECK (position ("1.1") == 0);
    CHECK (position ("1.2") == 960);
    CHECK (position ("2.1") == 3840);
    CHECK (position ("1.1.3") == 480);
    CHECK (position ("1.1.1") == 0);
    CHECK (position ("1.1+30") == 30);
    CHECK (position ("2.4.4+120") == 3840 + 3 * 960 + 3 * 240 + 120);
    CHECK (position ("1.2+960") == 1920); // ticks may run past the beat
}

TEST_CASE ("positions are written in their shortest form")
{
    CHECK (formatPosition (0, fourFour) == "1.1");
    CHECK (formatPosition (480, fourFour) == "1.1.3");
    CHECK (formatPosition (30, fourFour) == "1.1+30");
    CHECK (formatPosition (270, fourFour) == "1.1.2+30");
    CHECK (formatPosition (3840 * 11 + 960 * 2, fourFour) == "12.3");

    for (const auto* text : { "1.1", "1.1.3", "1.1+30", "2.4.4+120", "9.3.2+1" })
        CHECK (formatPosition (position (text), fourFour) == text);
}

TEST_CASE ("invalid positions explain what's wrong")
{
    const auto error = [] (std::string_view text) { return parsePosition (text, fourFour).error; };

    CHECK (error ("1.5").find ("beat 5 doesn't exist in 4/4 (beats are 1-4)") != std::string::npos);
    CHECK (error ("0.1").find ("bars start at 1") != std::string::npos);
    CHECK (error ("1.1.5").find ("sixteenth 5 doesn't exist") != std::string::npos);
    CHECK (error ("1").find ("at least a bar and a beat") != std::string::npos);
    CHECK (error ("1.1.1.1").find ("more than three numbers") != std::string::npos);
    CHECK (error ("1.1+x").find ("whole number of ticks") != std::string::npos);
    CHECK (error ("1..1").find ("a number is missing") != std::string::npos);
    CHECK (error ("1.1").empty());

    // Every message shows what valid input looks like.
    CHECK (error ("1.5").find ("e.g. '1.1'") != std::string::npos);
}

TEST_CASE ("pitches use C4 = 60 and keep their spelling")
{
    CHECK (pitch ("C4") == SpelledPitch { 60, Accidental::natural });
    CHECK (pitch ("C-1") == SpelledPitch { 0, Accidental::natural });
    CHECK (pitch ("G9") == SpelledPitch { 127, Accidental::natural });
    CHECK (pitch ("Eb2") == SpelledPitch { 39, Accidental::flat });
    CHECK (pitch ("D#2") == SpelledPitch { 39, Accidental::sharp });
    CHECK (pitch ("Bb-1") == SpelledPitch { 10, Accidental::flat });

    CHECK (formatPitch (39, Accidental::flat) == "Eb2");
    CHECK (formatPitch (39, Accidental::sharp) == "D#2");
    CHECK (formatPitch (39, Accidental::natural) == "D#2"); // sharps by default
    CHECK (formatPitch (0, Accidental::natural) == "C-1");
    CHECK (formatPitch (127, Accidental::natural) == "G9");
}

TEST_CASE ("Cb, Fb, B# and E# become the natural note")
{
    CHECK (pitch ("Cb4") == SpelledPitch { 59, Accidental::natural });
    CHECK (pitch ("B#3") == SpelledPitch { 60, Accidental::natural });
    CHECK (pitch ("E#2") == SpelledPitch { 41, Accidental::natural });
    CHECK (pitch ("Fb2") == SpelledPitch { 40, Accidental::natural });
    CHECK (formatPitch (59, Accidental::natural) == "B3");
}

TEST_CASE ("every MIDI pitch round-trips in both spellings")
{
    for (int p = 0; p <= 127; ++p)
    {
        for (const auto spelling : { Accidental::sharp, Accidental::flat })
        {
            const auto text = formatPitch (p, spelling);
            const auto parsed = parsePitch (text);
            REQUIRE_MESSAGE (parsed.value, text);
            CHECK (parsed.value->pitch == p);
        }
    }
}

TEST_CASE ("invalid pitches explain what's wrong")
{
    const auto error = [] (std::string_view text) { return parsePitch (text).error; };

    CHECK (error ("H2").find ("must start with a letter from A to G") != std::string::npos);
    CHECK (error ("c2").find ("must be uppercase") != std::string::npos);
    CHECK (error ("C").find ("octave from -1 to 9") != std::string::npos);
    CHECK (error ("C10").find ("octave from -1 to 9") != std::string::npos);
    CHECK (error ("C-2").find ("octave from -1 to 9") != std::string::npos);
    CHECK (error ("G#9").find ("above the MIDI range; the highest note is G9") != std::string::npos);
    CHECK (error ("Cb-1").find ("below the MIDI range; the lowest note is C-1") != std::string::npos);
    CHECK (error ("H2").find ("e.g. 'C2', 'F#4', 'Bb3'") != std::string::npos);
}

TEST_CASE ("lengths parse fractions, triplets and ticks")
{
    CHECK (length ("1/4") == 960);
    CHECK (length ("1/8") == 480);
    CHECK (length ("3/8") == 1440);
    CHECK (length ("1/1") == 3840);
    CHECK (length ("2/1") == 7680);
    CHECK (length ("1/8t") == 320);
    CHECK (length ("1/4t") == 640);
    CHECK (length ("1/12") == 320);
    CHECK (length ("250tk") == 250);
}

TEST_CASE ("lengths are written as the simplest fraction, then triplet, then ticks")
{
    CHECK (formatLength (960) == "1/4");
    CHECK (formatLength (1920) == "1/2");  // 2/4
    CHECK (formatLength (1440) == "3/8");
    CHECK (formatLength (3840) == "1/1");
    CHECK (formatLength (7680) == "2/1");
    CHECK (formatLength (60) == "1/64");
    CHECK (formatLength (300) == "5/64");
    CHECK (formatLength (320) == "1/8t"); // 1/12
    CHECK (formatLength (40) == "1/64t");
    CHECK (formatLength (30) == "30tk");  // 1/128 is finer than the canonical limit
    CHECK (formatLength (250) == "250tk");
    CHECK (formatLength (1) == "1tk");

    for (Ticks ticks = 1; ticks <= 4 * 3840; ++ticks)
        REQUIRE (length (formatLength (ticks)) == ticks);
}

TEST_CASE ("invalid lengths explain what's wrong")
{
    const auto error = [] (std::string_view text) { return parseLength (text).error; };

    CHECK (error ("1/7").find ("whole number of ticks (960 per beat)") != std::string::npos);
    CHECK (error ("0/4").find ("longer than zero") != std::string::npos);
    CHECK (error ("0tk").find ("longer than zero") != std::string::npos);
    CHECK (error ("1/0").find ("bottom number can't be zero") != std::string::npos);
    CHECK (error ("eighth").find ("needs a '/'") != std::string::npos);
    CHECK (error ("1/x").find ("both sides of the '/'") != std::string::npos);
    CHECK (error ("1/7").find ("'250tk'") != std::string::npos);
}

TEST_CASE ("velocities are v1 to v127")
{
    CHECK (parseVelocity ("v1").value == 1);
    CHECK (parseVelocity ("v127").value == 127);
    CHECK (formatVelocity (96) == "v96");

    CHECK (parseVelocity ("v0").error.find ("the lowest is v1") != std::string::npos);
    CHECK (parseVelocity ("v128").error.find ("the highest is v127") != std::string::npos);
    CHECK (parseVelocity ("96").error.find ("e.g. 'v96'") != std::string::npos);
    CHECK (parseVelocity ("v").error.find ("e.g. 'v96'") != std::string::npos);
}
