#pragma once

// Changing some of a clip's notes without sending all of them (SPEC §6.2,
// patch_clip). It matches notes, not lines: each note in an edit's old text
// must be in the clip with that length and velocity, and is removed; each
// note in its new text is added.

#include "core/notes/Notes.h"

#include <optional>
#include <span>
#include <string>

namespace hocket::notes
{
    struct PatchEdit
    {
        std::optional<std::string> oldText; // .notes lines whose notes are removed
        std::optional<std::string> newText; // .notes lines whose notes are added
    };

    struct PatchResult
    {
        std::optional<Clip> clip; // the clip with every edit applied; none on an error
        std::string error;        // what's wrong, naming the edit, with the clip's lines that show why
        std::string summary;      // "edit 1 removed 1 note and added 1; edit 2 added 4 notes"
    };

    // Applies the edits in order, each to the clip the one before left, all or
    // none. Positions are the clip's own; drum names are read with drumMap.
    // A trailing comment in old must be its stored line's; a line in old
    // without one matches whatever its stored line has. A comment stays with
    // its line while any of that line's notes remain. The clip that results
    // still has to be checked as write_clip checks any clip.
    PatchResult patch (const Clip& clip, std::span<const PatchEdit> edits, Meter meter, const DrumMap* drumMap);
}
