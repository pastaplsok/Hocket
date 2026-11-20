#pragma once

// A project's folder on disk, and saving into it (SPEC §4.5).
//
// A save writes only the files whose text changed. Each goes to a temporary
// file beside it first, all of them before any is swapped in; then they're
// swapped in, clip files before project.json. So a crash at any point leaves
// each file whole, old or new, and never leaves project.json naming a clip
// file that isn't there. A swap that fails puts back the files already
// swapped, so a failed save leaves the folder as it was.
//
// Clip files that were in the project, but that no clip uses any more, are
// moved to unused/, never deleted. Files Hocket never had in the project are
// never touched.

#include "core/commands/Storage.h"

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>

namespace hocket::storage
{
    // Where a save moves clip files no clip uses any more, keeping their
    // paths: "unused/clips/verse-2.notes".
    inline constexpr std::string_view unusedFolderName = "unused";

    // Added to a file's name for the temporary file a save writes beside it:
    // "clips/verse.notes.hocket-saving".
    inline constexpr std::string_view temporarySuffix = ".hocket-saving";

    class ProjectFolder final : public commands::Storage
    {
    public:
        // `loaded` is the project as read from the folder, or an empty
        // project for a new folder: its clip files are the ones Hocket has
        // had in the project so far.
        ProjectFolder (std::filesystem::path folder, const project::Project& loaded);

        const std::filesystem::path& path() const { return folder; }

        commands::SaveResult save (const project::Project& project, const commands::Author& author) override;

    private:
        std::filesystem::path folder;

        // Every clip file Hocket has had in the project here, loaded or
        // saved, that may still be on disk: the only files a save may move
        // to unused/.
        std::set<std::string, std::less<>> clipFiles;
    };

    // Saves an empty project (120 BPM, 4/4, no tracks) in `folder`, which
    // must be new or empty; a new folder is made, with any folders above it.
    // Returns what went wrong, or empty.
    std::string createProject (const std::filesystem::path& folder, const commands::Author& author);
}
