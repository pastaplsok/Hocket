#include "core/notes/DrumMap.h"
#include "core/notes/NotesSyntax.h"
#include "core/Suggestions.h"

#include <algorithm>
#include <cassert>

namespace hocket::notes
{
namespace
{
    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    bool isLowercaseLetter (char c)
    {
        return c >= 'a' && c <= 'z';
    }

    bool isDigit (char c)
    {
        return c >= '0' && c <= '9';
    }

    bool isNumber (std::string_view text)
    {
        return ! text.empty() && std::ranges::all_of (text, isDigit);
    }
}

//==============================================================================
bool DrumMap::add (std::string name, int note)
{
    assert (! drumNameProblem (name) && note >= 0 && note <= 127);

    if (names.contains (note) || notesByName.contains (name))
        return false;

    names.emplace (note, name);
    notesByName.emplace (std::move (name), note);
    return true;
}

std::optional<int> DrumMap::noteOf (std::string_view name) const
{
    if (const auto found = notesByName.find (name); found != notesByName.end())
        return found->second;

    return {};
}

const std::string* DrumMap::nameOf (int note) const
{
    const auto found = names.find (note);
    return found == names.end() ? nullptr : &found->second;
}

std::vector<std::string> DrumMap::nearestNames (std::string_view name) const
{
    // Within two edits, and closer than the name is long: "kik" is near "kick",
    // but "at" isn't near "hat".
    auto best = std::min<std::size_t> (2, name.size() > 1 ? name.size() - 1 : 0);
    std::vector<std::string> nearest;

    for (const auto& [note, candidate] : names)
    {
        const auto distance = editDistance (name, candidate);

        if (distance > best)
            continue;

        if (distance < best)
            nearest.clear();

        best = distance;
        nearest.push_back (candidate);
    }

    return nearest;
}

//==============================================================================
std::optional<std::string> drumNameProblem (std::string_view name)
{
    const auto examples = std::string ("e.g. 'kick', 'hat-closed', 'crash-18'");

    if (name.empty())
        return "a drum name can't be empty; drum names are like " + examples;

    const auto wellFormed = isLowercaseLetter (name.front()) && name.back() != '-' && name.find ("--") == std::string_view::npos
                         && std::ranges::all_of (name, [] (char c) { return isLowercaseLetter (c) || isDigit (c) || c == '-'; });

    if (! wellFormed)
        return singleQuoted (name) + " isn't a valid drum name: drum names are lowercase letters and digits in words joined by "
                                     "single hyphens, starting with a letter, "
             + examples;

    if (name.size() > DrumMap::maxNameLength)
        return singleQuoted (name) + " is too long for a drum name: they're at most " + std::to_string (DrumMap::maxNameLength) + " characters";

    // On a note line, each of these already means something else.
    const auto taken = [&name] (const std::string& what)
    {
        return singleQuoted (name) + " can't be a drum name: on a note line, " + what + ". Pick another name, e.g. 'kick', 'snare-rim'";
    };

    if (looksLikeLowercaseNoteName (name))
    {
        auto noteName = std::string (name);
        noteName[0] = static_cast<char> (noteName[0] - 'a' + 'A');
        return taken ("it reads as the note name " + noteName + ", written in lowercase");
    }

    if (looksLikeController (name))
        return taken ("'pb', 'at' and 'cc' with a number are controllers");

    if (name.front() == 'v' && isNumber (name.substr (1)))
        return taken ("'v' with a number is a velocity");

    if (name.starts_with ("ch") && isNumber (name.substr (2)))
        return taken ("'ch' with a number is a MIDI channel");

    return {};
}

const DrumMap& standardDrumMap()
{
    // SPEC §5.2's table: General MIDI's percussion notes, with Hocket's names.
    static const auto map = []
    {
        static constexpr const char* standardNames[] = {
            "kick2",          // 35
            "kick",           // 36
            "sidestick",      // 37
            "snare",          // 38
            "clap",           // 39
            "snare2",         // 40
            "tom-floor-low",  // 41
            "hat-closed",     // 42
            "tom-floor-high", // 43
            "hat-pedal",      // 44
            "tom-low",        // 45
            "hat-open",       // 46
            "tom-mid-low",    // 47
            "tom-mid-high",   // 48
            "crash",          // 49
            "tom-high",       // 50
            "ride",           // 51
            "china",          // 52
            "ride-bell",      // 53
            "tambourine",     // 54
            "splash",         // 55
            "cowbell",        // 56
            "crash2",         // 57
            "vibraslap",      // 58
            "ride2",          // 59
            "bongo-high",     // 60
            "bongo-low",      // 61
            "conga-high-mute", // 62
            "conga-high-open", // 63
            "conga-low",      // 64
            "timbale-high",   // 65
            "timbale-low",    // 66
            "agogo-high",     // 67
            "agogo-low",      // 68
            "cabasa",         // 69
            "maracas",        // 70
            "whistle-short",  // 71
            "whistle-long",   // 72
            "guiro-short",    // 73
            "guiro-long",     // 74
            "claves",         // 75
            "woodblock-high", // 76
            "woodblock-low",  // 77
            "cuica-mute",     // 78
            "cuica-open",     // 79
            "triangle-mute",  // 80
            "triangle-open",  // 81
        };

        DrumMap standard;
        int note = 35;

        for (const auto* name : standardNames)
            standard.add (name, note++);

        return standard;
    }();

    return map;
}
}
