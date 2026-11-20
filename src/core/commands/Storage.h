#pragma once

// Where the command bus saves the project (SPEC §4.5): its folder on disk
// (storage::ProjectFolder), or a test's fake. The bus decides when a save is
// needed and keeps track of what's unsaved; the storage writes the files, and
// keeps every version a save replaces.

#include "core/commands/Commands.h"
#include "core/project/Project.h"

#include <chrono>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace hocket::commands
{
    using SaveTime = std::chrono::sys_seconds;

    // Who saved a version of the project, and when.
    struct SaveInfo
    {
        SaveTime time {};
        std::string author; // a display name; empty for a version Hocket didn't save (from before Hocket)
        std::string name;   // what the save was named, if anything

        bool operator== (const SaveInfo&) const = default;
    };

    // A version of the project that a save replaced, kept whole, so it
    // restores by itself (SPEC §4.5).
    struct KeptSave
    {
        int number = 0;     // "#4": the order they were kept in
        SaveInfo info;      // the version's, from when it was saved
        std::string folder; // relative to the project folder: ".hocket/saves/4 2026-11-20 14-02 Lee"

        bool operator== (const KeptSave&) const = default;
    };

    // "2026-11-20 14:02", in local time.
    std::string localTimeText (SaveTime time);

    // "save #4, by Lee, 2026-11-20 14:02, \"before the bridge experiment\"";
    // "save #1, from before Hocket, 2026-11-18 09:15".
    std::string describe (const KeptSave& save);

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
        std::string message; // what was written and kept, or what went wrong and what that left on disk

        std::vector<std::string> written; // relative paths, in the order they were swapped in: clip files, then project.json
        std::vector<MovedFile> moved;      // to unused/
        std::optional<KeptSave> kept;      // the version this save replaced
    };

    class Storage
    {
    public:
        virtual ~Storage() = default;

        // Writes the project's files, first keeping the version they replace.
        // A failed save leaves the files on disk as they were.
        virtual SaveResult save (const project::Project& project, const Save& request, const Author& author) = 0;

        // Oldest first.
        virtual std::vector<KeptSave> keptSaves() = 0;

        // A kept save's project, read and checked like a project folder.
        virtual project::LoadResult loadKeptSave (const KeptSave& save) = 0;
    };
}
