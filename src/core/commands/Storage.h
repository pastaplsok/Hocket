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
        std::string author; // a display name; empty for a version Hocket didn't save
        std::string name;   // what the save was named, if anything

        // With no author: the version is a save of Hocket's with files changed
        // outside it since, kept by a save with overwrite, timed by the
        // latest change. Otherwise it's from before Hocket.
        bool changedOutside = false;

        // With no author: the version is a recovery copy, unsaved changes that
        // `--play --discard-recovery` filed with the kept saves rather than
        // play, so revert can still bring them back; timed by when it was
        // written (SPEC §4.5).
        bool discardedRecovery = false;

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
    // "save #1, from before Hocket, 2026-11-18 09:15"; "save #5, changed
    // outside Hocket, 2026-11-20 14:05"; "save #6, a discarded recovery
    // copy, 2026-11-21 10:30".
    std::string describe (const KeptSave& save);

    // "Lee", "before Hocket", "changed outside Hocket" or "discarded
    // recovery": who a version is by, as a kept save's folder name has it.
    std::string savedBy (const SaveInfo& info);

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
