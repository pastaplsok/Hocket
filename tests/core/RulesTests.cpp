// The rules shared by the project reader and the command bus. The reader's
// tests check their messages in place; these check the rules themselves.

#include "core/project/Rules.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace hocket;
using namespace hocket::project;

namespace
{
    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }
}

TEST_CASE ("one line of text is valid UTF-8 with no control characters, and not too long")
{
    CHECK (isOneLineText ("Bass", 100));
    CHECK (isOneLineText ("Größe — 低音 🎸", 100));
    CHECK (isOneLineText (std::string (100, 'a'), 100));

    CHECK_FALSE (isOneLineText ("", 100));
    CHECK_FALSE (isOneLineText (std::string (101, 'a'), 100));
    CHECK_FALSE (isOneLineText ("two\nlines", 100));
    CHECK_FALSE (isOneLineText ("a\ttab", 100));
    CHECK_FALSE (isOneLineText ("delete\x7f", 100));

    // Not UTF-8: a lone continuation byte, a truncated sequence, an overlong
    // '/', a UTF-16 surrogate, and a code point past U+10FFFF.
    CHECK_FALSE (isOneLineText ("\x80", 100));
    CHECK_FALSE (isOneLineText ("\xe2\x80", 100));
    CHECK_FALSE (isOneLineText ("\xc0\xaf", 100));
    CHECK_FALSE (isOneLineText ("\xed\xa0\x80", 100));
    CHECK_FALSE (isOneLineText ("\xf4\x90\x80\x80", 100));
}

TEST_CASE ("a track's name says what's wrong with it")
{
    CHECK_FALSE (trackNameProblem ("Bass \"low\" — main"));
    CHECK_FALSE (trackNameProblem (std::string (maxTrackNameLength, 'a')));

    CHECK (trackNameProblem ("") == "a track's name is one line of text, 1 to 100 bytes with no control characters, e.g. \"Bass\"; this one is empty");
    CHECK (contains (*trackNameProblem (std::string (101, 'a')), "; this one is 101 bytes long"));
    CHECK (contains (*trackNameProblem ("a\r\nb"), "; this one has a line break"));
    CHECK (contains (*trackNameProblem ("a\tb"), "; this one has a tab"));
    CHECK (contains (*trackNameProblem ("a\x01"), "; this one has a control character"));
    CHECK (contains (*trackNameProblem ("a\xff"), "; this one isn't valid UTF-8 text"));
}

TEST_CASE ("the tempo is 20 to 999 BPM, and the message shows the number given")
{
    CHECK_FALSE (tempoProblem (20.0));
    CHECK_FALSE (tempoProblem (999.0));
    CHECK_FALSE (tempoProblem (97.5));

    CHECK (tempoProblem (1000.0) == "1000 is out of range: the tempo is 20 to 999 BPM (quarter notes per minute), e.g. 120");
    CHECK (contains (*tempoProblem (19.99), "19.99 is out of range"));
    CHECK (contains (*tempoProblem (-5.0), "-5 is out of range"));
    CHECK (tempoProblem (std::numeric_limits<double>::quiet_NaN()));
    CHECK (tempoProblem (std::numeric_limits<double>::infinity()));
}

TEST_CASE ("IDs: each kind's prefix, then hyphenated words, at most 64 characters")
{
    CHECK_FALSE (idProblem ("trk-bass", IdKind::track));
    CHECK_FALSE (idProblem ("clp-bass-verse-2", IdKind::clip));
    CHECK_FALSE (idProblem ("dev-" + std::string (60, 'a'), IdKind::device));

    CHECK (idProblem ("clp-bass", IdKind::track) == "'clp-bass' is a clip ID; a track ID starts with 'trk-', e.g. 'trk-bass'");
    CHECK (idProblem ("bass", IdKind::clip) == "'bass' isn't a valid clip ID: clip IDs start with 'clp-', e.g. 'clp-bass-verse'");
    CHECK (contains (*idProblem ("dev-" + std::string (61, 'a'), IdKind::device), "is too long: IDs are at most 64 characters"));
    CHECK (contains (*idProblem ("trk-bass--2", IdKind::track), "after 'trk-', use lowercase letters and digits in words joined by single hyphens"));
    CHECK (idProblem ("trk-", IdKind::track));
}

TEST_CASE ("track and device types, and where a drum sampler goes")
{
    CHECK (parseTrackType ("drums").value == TrackType::drums);
    CHECK (parseTrackType ("audio").error == "\"audio\" tracks aren't supported yet; so far there are \"midi\" and \"drums\" tracks");

    CHECK (parseDeviceType ("hocket:drum-sampler").value == DeviceType::drumSampler);
    CHECK (contains (parseDeviceType ("tracktion:4osc").error, "is now written \"hocket:synth\""));
    CHECK (contains (parseDeviceType ("vst3:serum").error, "\"vst3:serum\" isn't available"));

    CHECK_FALSE (devicePlacementProblem (DeviceType::synth, TrackType::drums));
    CHECK_FALSE (devicePlacementProblem (DeviceType::drumSampler, TrackType::drums));
    CHECK (devicePlacementProblem (DeviceType::drumSampler, TrackType::midi)
           == "a \"hocket:drum-sampler\" goes on a \"drums\" track, whose drum map names the kit's pieces; this is a \"midi\" track");

    CHECK_FALSE (kitNameProblem ("acoustic-2"));
    CHECK (kitNameProblem ("Acoustic"));
    CHECK (kitNameProblem (std::string (65, 'a')));
}

TEST_CASE ("a drum map is built from its entries in order, and each one that can't go in says why")
{
    const std::vector<DrumMapEntry> entries {
        { "kick", 36 },
        { "Snare", 38 },  // not a valid name
        { "snare", 128 }, // not a MIDI note
        { "rim", 36 },    // kick's note
        { "kick", 35 },   // kick again
        { "Snare", 40 },  // an invalid name, again: still a name that appears twice
        { "hat", 42 },
    };

    notes::DrumMap map;
    const auto problems = buildDrumMap (entries, map);

    CHECK (map.byNote() == std::map<int, std::string> { { 36, "kick" }, { 42, "hat" } });
    REQUIRE (problems.size() == 5);

    CHECK (problems[0].entry == 1);
    CHECK (contains (problems[0].message, "'Snare' isn't a valid drum name"));
    CHECK_FALSE (problems[0].aboutNote);

    CHECK (problems[1].entry == 2);
    CHECK (problems[1].message == "\"snare\" should be a MIDI note number from 0 to 127, e.g. 36");
    CHECK (problems[1].aboutNote);

    CHECK (problems[2].entry == 3);
    CHECK (contains (problems[2].message, "\"rim\" and \"kick\" are both note 36, but a note has one name in a drum map"));
    CHECK (problems[2].aboutNote);

    CHECK (problems[3].entry == 4);
    CHECK (problems[3].message == "\"kick\" appears twice; a name stands for one note, so keep only one");
    CHECK_FALSE (problems[3].aboutNote);

    CHECK (problems[4].entry == 5);
    CHECK (contains (problems[4].message, "\"Snare\" appears twice"));
}

TEST_CASE ("clip files are clips/<name>.notes, and every clip ends by the end of bar 999")
{
    CHECK_FALSE (clipFileProblem ("clips/bass-verse.notes"));
    CHECK (clipFileProblem ("clips/.notes"));
    CHECK (clipFileProblem ("clips/Bass.notes"));
    CHECK (clipFileProblem ("bass.notes"));

    const notes::Meter fourFour;
    const auto bar = fourFour.ticksPerBar();
    CHECK_FALSE (songLengthProblem (fourFour, 997 * bar, 999 * bar));
    CHECK (songLengthProblem (fourFour, 998 * bar, 1000 * bar)
           == "it runs from 999.1 to 1001.1, but a song is at most 999 bars long, so every clip ends by 1000.1; start it at 998.1 or earlier");

    const notes::Meter sixEight { 6, 8 };
    CHECK (contains (*songLengthProblem (sixEight, 998 * sixEight.ticksPerBar() + 480, 999 * sixEight.ticksPerBar() + 480),
                     "it runs from 999.2 to 1000.2, but a song is at most 999 bars long, so every clip ends by 1000.1; "
                     "start it at 999.1 or earlier"));
}
