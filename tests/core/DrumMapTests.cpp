#include "core/notes/DrumMap.h"

#include <doctest/doctest.h>

using namespace hocket::notes;

namespace
{
    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }
}

TEST_CASE ("drum names are lowercase words joined by hyphens, starting with a letter")
{
    for (const auto* name : { "kick", "hat-closed", "crash-18", "tom-floor-low", "kick2", "a", "v", "ch", "cc", "pbx", "at2x", "h2" })
    {
        CAPTURE (name);
        CHECK_FALSE (drumNameProblem (name));
    }

    CHECK (drumNameProblem (std::string (DrumMap::maxNameLength, 'x')) == std::nullopt);
    CHECK (contains (*drumNameProblem (std::string (DrumMap::maxNameLength + 1, 'x')), "is too long for a drum name: they're at most 32 characters"));

    for (const auto* name : { "Kick", "hat closed", "hat_closed", "-kick", "kick-", "hat--open", "808", "2kick", "kick!", "é" })
    {
        CAPTURE (name);
        const auto problem = drumNameProblem (name);
        REQUIRE (problem);
        CHECK (contains (*problem, "isn't a valid drum name: drum names are lowercase letters and digits in words joined by single hyphens, "
                                   "starting with a letter, e.g. 'kick', 'hat-closed', 'crash-18'"));
    }

    CHECK (contains (*drumNameProblem (""), "a drum name can't be empty"));
}

TEST_CASE ("a drum name can't read as anything else on a note line")
{
    struct Case
    {
        std::string_view name;
        std::string_view message;
    };

    const Case cases[] = {
        { "c2", "'c2' can't be a drum name: on a note line, it reads as the note name C2, written in lowercase" },
        { "eb3", "it reads as the note name Eb3" },
        { "bb-1", "it reads as the note name Bb-1" },
        { "g9", "it reads as the note name G9" },
        { "cc74", "'cc74' can't be a drum name: on a note line, 'pb', 'at' and 'cc' with a number are controllers" },
        { "pb", "are controllers" },
        { "at", "are controllers" },
        { "v96", "'v96' can't be a drum name: on a note line, 'v' with a number is a velocity" },
        { "v1", "is a velocity" },
        { "ch2", "'ch2' can't be a drum name: on a note line, 'ch' with a number is a MIDI channel" },
        { "ch16", "is a MIDI channel" },
    };

    for (const auto& c : cases)
    {
        CAPTURE (c.name);
        const auto problem = drumNameProblem (c.name);
        REQUIRE (problem);
        CHECK_MESSAGE (contains (*problem, c.message), *problem);
        CHECK (contains (*problem, "Pick another name"));
    }
}

TEST_CASE ("a drum map has one name per note and one note per name")
{
    DrumMap map;
    CHECK (map.empty());
    CHECK (map.add ("kick", 36));
    CHECK (map.add ("snare", 38));

    CHECK_FALSE (map.add ("kick", 35));  // the name is taken
    CHECK_FALSE (map.add ("bass-drum", 36)); // so is the note
    CHECK (map.size() == 2);

    CHECK (map.noteOf ("kick") == 36);
    CHECK (map.noteOf ("bass-drum") == std::nullopt);
    REQUIRE (map.nameOf (38) != nullptr);
    CHECK (*map.nameOf (38) == "snare");
    CHECK (map.nameOf (37) == nullptr);

    // By note, whatever order they were added in.
    DrumMap reversed;
    reversed.add ("snare", 38);
    reversed.add ("kick", 36);
    CHECK (reversed == map);
    CHECK (reversed.byNote().begin()->second == "kick");
}

TEST_CASE ("did you mean: the nearest names, within two edits")
{
    DrumMap map;
    map.add ("kick", 36);
    map.add ("hat-closed", 42);
    map.add ("hat-open", 46);
    map.add ("crash", 49);
    map.add ("crash2", 57);

    CHECK (map.nearestNames ("kik") == std::vector<std::string> { "kick" });
    CHECK (map.nearestNames ("hat-clsed") == std::vector<std::string> { "hat-closed" });
    CHECK (map.nearestNames ("crash3") == std::vector<std::string> { "crash", "crash2" });
    CHECK (map.nearestNames ("hihat").empty());
    CHECK (map.nearestNames ("k").empty()); // too short for a guess to mean much
}

TEST_CASE ("the standard names are SPEC §5.2's, on General MIDI's notes 35 to 81")
{
    const auto& standard = standardDrumMap();
    CHECK (standard.size() == 47);
    CHECK (standard.byNote().begin()->first == 35);
    CHECK (standard.byNote().rbegin()->first == 81);

    // Every name follows the rules.
    for (const auto& [note, name] : standard.byNote())
    {
        CAPTURE (name);
        CHECK_FALSE (drumNameProblem (name));
    }

    // Against the table, a few from each column and the ends.
    const std::pair<int, std::string_view> table[] = {
        { 35, "kick2" }, { 36, "kick" }, { 37, "sidestick" }, { 38, "snare" }, { 39, "clap" }, { 40, "snare2" },
        { 42, "hat-closed" }, { 44, "hat-pedal" }, { 46, "hat-open" }, { 49, "crash" }, { 51, "ride" }, { 52, "china" },
        { 53, "ride-bell" }, { 55, "splash" }, { 56, "cowbell" }, { 57, "crash2" }, { 59, "ride2" }, { 62, "conga-high-mute" },
        { 67, "agogo-high" }, { 71, "whistle-short" }, { 75, "claves" }, { 78, "cuica-mute" }, { 81, "triangle-open" },
    };

    for (const auto& [note, name] : table)
    {
        CAPTURE (note);
        REQUIRE (standard.nameOf (note) != nullptr);
        CHECK (*standard.nameOf (note) == name);
    }
}
