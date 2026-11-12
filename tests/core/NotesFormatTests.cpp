#include "core/notes/Notes.h"

#include <doctest/doctest.h>

using namespace hocket::notes;

namespace
{
    Clip parseOrFail (std::string_view text)
    {
        auto result = parse (text);

        for (const auto& error : result.errors)
            MESSAGE (formatDiagnostic (error, "input"));

        REQUIRE (result.clip);
        return std::move (*result.clip);
    }

    std::string canonical (std::string_view text)
    {
        return write (parseOrFail (text));
    }

    // The single error a line of input produces.
    Diagnostic onlyError (std::string_view text)
    {
        const auto result = parse (text);
        CHECK_FALSE (result.clip);
        REQUIRE (result.errors.size() == 1);
        return result.errors.front();
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    // A canonical file exercising every feature so far.
    constexpr std::string_view canonicalClip = R"(# bass-verse.notes — 4 bars
# second header line
@length 4bars  # four bars

1.1      C2    1/8   v96
1.1.3    C2    1/16  v70
1.2      Eb2   1/4   v90
1.4      G1    1/4   v85

# the chord
2.1      [C3 Eb3 G3]  1/2   v80  # minor
2.1      C4    1/8   v100
2.3+30   C2    1/8t  v25
2.4.4+120  D#2   250tk v127

3.1      C-1   1/1   v1
#
# trailing comments
)";
}

TEST_CASE ("canonical files round-trip byte for byte")
{
    CHECK (canonical (canonicalClip) == canonicalClip);
    CHECK (canonical ("@length 1bar\n") == "@length 1bar\n");
    CHECK (canonical ("@length 1bar\n\n1.1      C4    1/4   v100\n") == "@length 1bar\n\n1.1      C4    1/4   v100\n");
}

TEST_CASE ("the parser builds one note per pitch")
{
    const auto clip = parseOrFail (canonicalClip);

    CHECK (clip.lengthInBars == 4);
    CHECK (clip.header == std::vector<std::string> { " bass-verse.notes — 4 bars", " second header line" });
    CHECK (clip.lengthComments.trailing == " four bars");
    REQUIRE (clip.notes.size() == 11);

    const auto& eb2 = clip.notes[2];
    CHECK (eb2.start == 960);
    CHECK (eb2.length == 960);
    CHECK (eb2.pitch == 39);
    CHECK (eb2.velocity == 90);
    CHECK (eb2.spelling == Accidental::flat);

    // A chord line's comments belong to its first note.
    CHECK (clip.notes[4].pitch == 48);
    CHECK (clip.notes[4].comments.leading == std::vector<std::string> { " the chord" });
    CHECK (clip.notes[4].comments.trailing == " minor");
    CHECK (clip.notes[5].comments == Comments {});

    CHECK (clip.footer == std::vector<std::string> { "", " trailing comments" });
}

TEST_CASE ("loose input is rewritten in canonical form")
{
    const auto messy = "\xEF\xBB\xBF# messy\r\n"
                       "1.4 G1 1/4 v85\r\n"
                       "   # about the chord\r\n"
                       "2.1\t[G3 C3]\t2/4\tv80 # chord part one\r\n"
                       "\r\n"
                       "2.1 Eb3 1/2 v80   # part two   \r\n"
                       "1.1.1+0 C2 1/8 v96\r\n"
                       "@length 4bars\r\n";

    const auto expected = "# messy\n"
                          "@length 4bars\n"
                          "\n"
                          "1.1      C2    1/8   v96\n"
                          "1.4      G1    1/4   v85\n"
                          "\n"
                          "# about the chord\n"
                          "2.1      [C3 Eb3 G3]  1/2   v80  # chord part one; part two\n";

    CHECK (canonical (messy) == expected);
    CHECK (canonical (expected) == expected);
}

TEST_CASE ("only notes sharing position, length and velocity form a chord")
{
    const auto input = "1.1 G3 1/4 v80\n"
                       "1.1 C3 1/2 v80\n"
                       "1.1 E3 1/4 v80\n"
                       "1.1 C2 1/4 v60\n";

    const auto expected = "@length 1bar\n"
                          "\n"
                          "1.1      C2    1/4   v60\n"
                          "1.1      C3    1/2   v80\n"
                          "1.1      [E3 G3]  1/4   v80\n";

    CHECK (canonical (input) == expected);
}

TEST_CASE ("the clip length defaults to the bars that hold every note")
{
    CHECK (parseOrFail ("").lengthInBars == 1);
    CHECK (parseOrFail ("1.4 C2 1/4 v96").lengthInBars == 1);
    CHECK (parseOrFail ("5.1 C2 1/4 v96").lengthInBars == 5);
    CHECK (canonical ("2.1 C2 1/4 v96") == "@length 2bars\n\n2.1      C2    1/4   v96\n");
}

TEST_CASE ("a clip is at most 999 bars long")
{
    CHECK (maxLengthInBars == 999);
    CHECK (parseOrFail ("@length 999bars").lengthInBars == 999);
    CHECK (parseOrFail ("999.4 C2 1/4 v96").lengthInBars == 999);

    // A typo in a bar number would otherwise make a clip, and a loop, of thousands of bars.
    for (const auto* text : { "1000.1 C2 1/4 v96", "@length 999bars\n1000.1 C2 1/4 v96" })
    {
        INFO (text);
        const auto tooFar = parse (text);
        REQUIRE (tooFar.errors.size() == 1);
        CHECK (tooFar.errors[0].message == "C2 at 1000.1 is past bar 999: a clip is at most 999 bars long, so move the note");
    }
}

TEST_CASE ("notes may run past the end of the clip but not start there")
{
    CHECK (parseOrFail ("@length 1bar\n1.4 C2 1/1 v96").notes.size() == 1);

    const auto error = onlyError ("@length 1bar\n  2.1 C2 1/8 v96");
    CHECK (error.line == 2);
    CHECK (error.column == 3);
    CHECK (contains (error.message, "C2 at 2.1 starts at or after the end of the clip (@length 1bar ends at 2.1)"));
}

TEST_CASE ("a '#' inside a note name isn't a comment")
{
    const auto clip = parseOrFail ("1.1 F#4 1/4 v90 # sharp\n1.2 [C#3 F#3] 1/4 v90");
    REQUIRE (clip.notes.size() == 3);
    CHECK (clip.notes[0].pitch == 66);
    CHECK (clip.notes[0].comments.trailing == " sharp");
    CHECK (clip.notes[2].pitch == 54);
}

TEST_CASE ("comments set through the API can't break the format")
{
    Clip clip;
    clip.header = { " line one\nline two  " };
    clip.notes.push_back ({ 0, 960, 60, 100, Accidental::natural, { {}, " tail \r\n" } });

    const auto text = write (clip);
    CHECK (text == "# line one line two\n@length 1bar\n\n1.1      C4    1/4   v100  # tail\n");
    CHECK (canonical (text) == text);
}

TEST_CASE ("errors give line, column, the problem, and a valid example")
{
    struct Case
    {
        std::string_view input;
        int line;
        int column;
        std::string_view message;
    };

    const Case cases[] = {
        { "1.5 C2 1/8 v96", 1, 1, "'1.5' isn't a valid position: beat 5 doesn't exist in 4/4" },
        { "1.1 H2 1/8 v96", 1, 5, "'H2' isn't a note name: it must start with a letter from A to G" },
        { "1.1 c2 1/8 v96", 1, 5, "'c2' isn't a note name: note names start with an uppercase letter, e.g. 'C2'" },
        { "1.1 kick 1/16 v110", 1, 5, "'kick' looks like a drum name, but this clip's track has no drum map, so write a note name such as 'C2'. "
                                      "Drum names work on \"drums\" tracks" },
        { "1.1 60 1/4 v96", 1, 5, "'60' isn't a note name: clips write notes by name, not number. Note 60 is 'C4' (C4 is 60)" },
        { "2.1 cc74 ramp 40->100 until 3.1", 1, 5, "controller lines ('cc74', 'pb', 'at') aren't supported yet (they come in Milestone 6)" },
        { "3.3 pb +0.5 until 3.4", 1, 5, "controller lines" },
        { "2.1 at ramp 0->100 until 3.1", 1, 5, "controller lines" },
        { "1.1 C2 1/7 v96", 1, 8, "'1/7' isn't a valid length: it doesn't come out to a whole number of ticks" },
        { "1.1 C2 1/8 v0", 1, 12, "'v0' isn't a valid velocity: the lowest is v1" },
        { "1.1 C2 1/8 96", 1, 12, "'96' isn't a velocity" },
        { "1.1 C2 1/8", 1, 11, "expected a velocity after the length, e.g. 'v96'" },
        { "1.1 C2", 1, 7, "expected a length after the pitch, e.g. '1/8'" },
        { "1.1", 1, 4, "expected a pitch after the position" },
        { "1.1 C2 1/8 v96 ch2", 1, 16, "per-note MIDI channels ('ch2') aren't supported yet (they come in Milestone 3)" },
        { "1.1 C2 1/8 v96 loud", 1, 16, "unexpected 'loud' after the velocity" },
        { "1.1 [C3 E3 1/8 v96", 1, 5, "this chord has no closing ']'" },
        { "1.1 [] 1/8 v96", 1, 5, "this chord is empty" },
        { "1.1 [C3 X3] 1/8 v96", 1, 9, "'X3' isn't a note name" },
        { "1.1 G#9 1/8 v96", 1, 5, "'G#9' is above the MIDI range; the highest note is G9" },
        { "@swing 1/16 58%", 1, 1, "'@swing' isn't supported yet (it comes in Milestone 3)" },
        { "@ch 2", 1, 1, "'@ch' isn't supported yet (it comes in Milestone 3)" },
        { "@tempo 120", 1, 1, "unknown directive '@tempo'; the only one so far is '@length'" },
        { "@length", 1, 8, "@length needs a value, e.g. '@length 4bars'" },
        { "@length 4beats", 1, 9, "'4beats' isn't a valid @length: clip lengths are a whole number of bars" },
        { "@length 0bars", 1, 9, "isn't a valid @length" },
        { "@length 1000bars", 1, 9, "'1000bars' is too long: a clip is at most 999 bars" },
        { "@length 99999999bars", 1, 9, "'99999999bars' is too long: a clip is at most 999 bars" },
        { "1.1 C2 1/4 v96\n100000.1 C2 1/4 v96", 2, 1, "C2 at 100000.1 is past bar 999: a clip is at most 999 bars long" },
        { "@length 4 bars", 1, 11, "unexpected 'bars' after the @length value" },
        { "@length 4bars\n@length 2bars", 2, 1, "@length is set twice; it was already set on line 1" },
        { "1.1 C2 1/8 v96\n1.1 C2 1/4 v80", 2, 5, "duplicate note: C2 at 1.1 is already on line 1" },
        { "1.1 [C3 C3] 1/8 v96", 1, 9, "duplicate note: C3 at 1.1 is already on line 1" },
    };

    for (const auto& c : cases)
    {
        CAPTURE (c.input);
        const auto error = onlyError (c.input);
        CHECK (error.line == c.line);
        CHECK (error.column == c.column);
        CHECK_MESSAGE (contains (error.message, c.message), error.message);
    }
}

TEST_CASE ("every error in the file is reported, in line order")
{
    const auto result = parse ("1.1 C2 1/8 v96\n"
                               "1.5 C2 1/8 v96\n"
                               "1.2 C2 1/8 v96\n"
                               "1.1 C2 1/8 v96\n"
                               "1.3 H2 1/8 v96\n");
    CHECK_FALSE (result.clip);
    REQUIRE (result.errors.size() == 3);
    CHECK (result.errors[0].line == 2);
    CHECK (result.errors[1].line == 4); // duplicate, found after the whole file is read
    CHECK (result.errors[2].line == 5);
}

TEST_CASE ("a flood of errors stops at twenty")
{
    std::string input;

    for (int i = 0; i < 30; ++i)
        input += "1.9 C2 1/8 v96\n";

    const auto result = parse (input);
    REQUIRE (result.errors.size() == 21);
    CHECK (contains (result.errors.back().message, "stopping after 20 errors"));
}

//==============================================================================
namespace
{
    DrumMap smallKit()
    {
        DrumMap map;
        map.add ("kick", 36);
        map.add ("sidestick", 37);
        map.add ("snare", 38);
        map.add ("hat-closed", 42);
        map.add ("crash", 49);
        map.add ("crash2", 57);
        return map;
    }

    Clip parseDrumsOrFail (std::string_view text, const DrumMap& map)
    {
        auto result = parse (text, {}, &map);

        for (const auto& error : result.errors)
            MESSAGE (formatDiagnostic (error, "input"));

        REQUIRE (result.clip);
        return std::move (*result.clip);
    }

    Diagnostic onlyDrumError (std::string_view text, const DrumMap& map)
    {
        const auto result = parse (text, {}, &map);
        CHECK_FALSE (result.clip);
        REQUIRE (result.errors.size() == 1);
        return result.errors.front();
    }

    constexpr std::string_view canonicalDrums = R"(# a beat
@length 1bar

1.1      [kick hat-closed]  1/16  v100
1.1.3    hat-closed  1/16  v70
1.2      snare 1/16  v100  # backbeat
1.2.3    snare 1/16  v28
1.3      kick  1/16  v110
1.4      sidestick  1/16  v90
1.4      A#5   1/16  v80
)";
}

TEST_CASE ("on a drums track, clips name the drum map's pieces, and round-trip byte for byte")
{
    const auto map = smallKit();
    const auto clip = parseDrumsOrFail (canonicalDrums, map);

    REQUIRE (clip.notes.size() == 8);
    CHECK (clip.notes[0].pitch == 36);
    CHECK (clip.notes[1].pitch == 42);
    CHECK (clip.notes[3].pitch == 38);
    CHECK (clip.notes[3].comments.trailing == " backbeat");

    // A named note on a black key is kept sharp, so it falls back to A#-style names.
    CHECK (clip.notes[6].pitch == 37);
    CHECK (clip.notes[6].spelling == Accidental::sharp);

    // A note the map doesn't name stays a note name.
    CHECK (clip.notes[7].pitch == 82);

    CHECK (write (clip, {}, &map) == canonicalDrums);

    // Without the map, the same notes are note names.
    CHECK (contains (write (clip), "1.1      [C2 F#2]  1/16  v100\n"));
}

TEST_CASE ("on a drums track, every note the map names is written by its name")
{
    const auto map = smallKit();

    // As Cb4 becomes B3: the map's name is the canonical spelling.
    CHECK (write (parseDrumsOrFail ("1.1 D2 1/4 v100\n1.2 Db2 1/4 v100\n1.3 C#2 1/4 v90\n1.4 [C2 F#2] 1/8 v80", map), {}, &map)
           == "@length 1bar\n\n1.1      snare 1/4   v100\n1.2      sidestick  1/4   v100\n1.3      sidestick  1/4   v90\n"
              "1.4      [kick hat-closed]  1/8   v80\n");

    // The model is the same however the note was written.
    CHECK (parseDrumsOrFail ("1.1 Db2 1/4 v100", map) == parseDrumsOrFail ("1.1 sidestick 1/4 v100", map));

    // Notes the map doesn't name keep their spelling.
    CHECK (write (parseDrumsOrFail ("1.1 Bb5 1/4 v100\n1.2 A#5 1/4 v100", map), {}, &map)
           == "@length 1bar\n\n1.1      Bb5   1/4   v100\n1.2      A#5   1/4   v100\n");
}

TEST_CASE ("drum names in errors, and errors about drum names")
{
    const auto map = smallKit();

    // A note named twice, by name and by note name, is one note.
    const auto duplicate = onlyDrumError ("1.1 kick 1/16 v100\n1.1 C2 1/8 v90", map);
    CHECK (duplicate.line == 2);
    CHECK (contains (duplicate.message, "duplicate note: kick at 1.1 is already on line 1"));

    CHECK (contains (onlyDrumError ("@length 1bar\n2.1 snare 1/16 v100", map).message, "snare at 2.1 starts at or after the end of the clip"));

    struct Case
    {
        std::string_view input;
        int column;
        std::string_view message;
    };

    const Case cases[] = {
        { "1.1 kik 1/16 v100", 5, "'kik' isn't in this track's drum map (did you mean 'kick'?). Its names are kick, sidestick, snare, "
                                  "hat-closed, crash, crash2. A note the map doesn't name is written as a note name, e.g. 'A#5'" },
        { "1.1 crash3 1/16 v100", 5, "'crash3' isn't in this track's drum map (did you mean 'crash' or 'crash2'?)" },
        { "1.1 crsh 1/16 v100", 5, "(did you mean 'crash'?)" },
        { "1.1 bell 1/16 v100", 5, "'bell' isn't in this track's drum map. Its names are kick" },
        { "1.1 Kick 1/16 v100", 5, "'Kick' isn't a drum name: drum names are lowercase, so write 'kick'" },
        { "1.1 36 1/16 v100", 5, "'36' isn't a note name: clips write notes by name, not number. Note 36 is 'kick' in this track's drum map, or 'C2'" },
        { "1.1 c2 1/16 v100", 5, "'c2' isn't a note name: note names start with an uppercase letter, e.g. 'C2'" },
        { "1.1 cc7 1/16 v100", 5, "controller lines ('cc74', 'pb', 'at') aren't supported yet" },
        { "1.1 [kick snre] 1/16 v100", 11, "'snre' isn't in this track's drum map (did you mean 'snare'?)" },
    };

    for (const auto& c : cases)
    {
        CAPTURE (c.input);
        const auto error = onlyDrumError (c.input, map);
        CHECK (error.column == c.column);
        CHECK_MESSAGE (contains (error.message, c.message), error.message);
    }

    // A map can have dozens of names, so only the first such error lists them.
    const auto twice = parse ("1.1 bell 1/16 v100\n1.2 gong 1/16 v100", {}, &map);
    REQUIRE (twice.errors.size() == 2);
    CHECK (contains (twice.errors[0].message, "Its names are kick, sidestick"));
    CHECK (twice.errors[1].message == "'gong' isn't in this track's drum map; the first error like this one lists its names");

    const DrumMap empty;
    CHECK (contains (onlyDrumError ("1.1 kick 1/16 v100", empty).message,
                     "'kick' isn't in this track's drum map, which is empty, so write a note name such as 'C2'"));
}

TEST_CASE ("diagnostics are formatted as source:line:column: message")
{
    const Diagnostic diagnostic { 6, 10, "'1/7' isn't a valid length" };
    CHECK (formatDiagnostic (diagnostic, "clips/bass.notes") == "clips/bass.notes:6:10: '1/7' isn't a valid length");
}
