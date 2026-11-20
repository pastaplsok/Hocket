#pragma once

// Where the command bus saves the project (SPEC §4.5): its folder on disk
// (storage::ProjectFolder), or a test's fake. The bus decides when a save is
// needed and keeps track of what's unsaved; the storage writes the files.

#include "core/commands/Commands.h"
#include "core/project/Project.h"

#include <string>
#include <vector>

namespace hocket::commands
{
    // A clip file no clip uses any more, moved out of the way on save.
    struct MovedFile
    {
        std::string from; // relative to the project folder: "clips/verse-2.notes"
        std::string to;   // "unused/clips/verse-2.notes"

        bool operator== (const MovedFile&) const = default;
    };

    struct SaveResult
    {
        bool ok = false;
        std::string message; // what was written, or what went wrong and what that left on disk

        std::vector<std::string> written; // relative paths, in the order they were swapped in: clip files, then project.json
        std::vector<MovedFile> moved;      // to unused/
    };

    class Storage
    {
    public:
        virtual ~Storage() = default;

        // Writes the project's files. A failed save leaves the files on disk as they were.
        virtual SaveResult save (const project::Project& project, const Author& author) = 0;
    };
}
