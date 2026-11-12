#pragma once

// Validating each command against the project, and planning the edits that
// carry it out. Internal to the bus, which records what a plan does.

#include "core/commands/Edits.h"

namespace hocket::commands::planning
{
    // SongCommands.cpp
    edits::Plan plan (const project::Project& project, const SetTempo& command);
    edits::Plan plan (const project::Project& project, const SetTimeSignature& command);

    // TrackCommands.cpp
    edits::Plan plan (const project::Project& project, const AddTrack& command);
    edits::Plan plan (const project::Project& project, const RemoveTrack& command);
    edits::Plan plan (const project::Project& project, const RenameTrack& command);
    edits::Plan plan (const project::Project& project, const MoveTrack& command);
    edits::Plan plan (const project::Project& project, const SetMix& command);

    // ClipCommands.cpp
    edits::Plan plan (const project::Project& project, const WriteClip& command);

    // A refusal: what was being done, why not, and how to fix it.
    edits::Plan refused (std::string message);

    // Valid, but it changes nothing: says so, and adds no history entry.
    edits::Plan unchanged (std::string message);

    // "'clp-a'"
    std::string singleQuoted (std::string_view text);
    std::string doubleQuoted (std::string_view text);

    // "1bar", "4bars", as @length writes them.
    std::string barsText (int bars);

    // "1 note", "3 notes"
    std::string countText (std::size_t count, std::string_view noun);

    // "a", "a and b", "a, b and c"
    std::string listText (const std::vector<std::string>& items);
}
