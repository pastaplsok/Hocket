#pragma once

// Drum maps (SPEC §5.2): the names a drums track's clips use for its notes,
// e.g. "kick" for 36. The map lives on the track, in project.json.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::notes
{
    // Names to MIDI notes: one name per note and one note per name, so the
    // parser can read a name and the writer knows which name to write.
    class DrumMap
    {
    public:
        static constexpr std::size_t maxNameLength = 32;

        // Adds a piece. Returns false, changing nothing, if the map already has
        // the name or the note. The name must be valid (drumNameProblem), and
        // the note 0 to 127.
        bool add (std::string name, int note);

        std::optional<int> noteOf (std::string_view name) const;

        // nullptr when the map doesn't name the note.
        const std::string* nameOf (int note) const;

        // Note to name, by note: the canonical order (SPEC §4.4).
        const std::map<int, std::string>& byNote() const { return names; }

        bool empty() const { return names.empty(); }
        std::size_t size() const { return names.size(); }

        // The names nearest a misspelt one, for "did you mean": empty if none is close.
        std::vector<std::string> nearestNames (std::string_view name) const;

        bool operator== (const DrumMap& other) const { return names == other.names; }

    private:
        std::map<int, std::string> names;                    // note -> name
        std::map<std::string, int, std::less<>> notesByName; // name -> note
    };

    // Why a name can't be in a drum map, or nullopt if it can. A name is
    // lowercase letters and digits in words joined by single hyphens, starting
    // with a letter, at most DrumMap::maxNameLength bytes. It mustn't read as
    // anything else on a note line: a note name ("c2"), a controller ("cc74",
    // "pb", "at"), a velocity ("v96") or a MIDI channel ("ch2").
    std::optional<std::string> drumNameProblem (std::string_view name);

    // General MIDI's percussion notes, 35 to 81, with Hocket's standard names (SPEC §5.2).
    const DrumMap& standardDrumMap();
}
