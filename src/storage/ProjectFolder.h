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
// Before a save changes anything, it keeps the version on disk whole, in
// .hocket/saves/<number> <time> <author>[ <name>]/: project.json, its clip
// files, and .hocket/save.json, which says who saved that version and when.
// Each kept save is a project folder of its own, so it loads, and restores
// by hand, by itself. Nothing in .hocket/ is ever part of the project.
//
// Clip files that were in the project, but that no clip uses any more, are
// moved to unused/, never deleted. Files Hocket never had in the project are
// never touched.

#include "core/commands/Storage.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
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

    // Who saved the files beside it, and when: in a project folder, and in
    // each kept save's.
    inline constexpr std::string_view saveInfoFile = ".hocket/save.json";

    // The kept saves, a folder each.
    inline constexpr std::string_view keptSavesFolder = ".hocket/saves";

    // How long a kept save's folder name may be, in UTF-16 code units, so its
    // files' paths stay well inside Windows' 260.
    inline constexpr std::size_t maxKeptSaveNameLength = 64;

    // "4 2026-11-20 14-02 Lee before the bridge experiment": the number, the
    // local time it was saved, the author ("before Hocket" for a version Hocket
    // didn't save) and its name, if any. Valid on Windows whatever the author
    // or name hold: characters Windows refuses in a name (<>:"/\|?* and control
    // characters) become spaces, runs of spaces one, invalid UTF-8 is dropped,
    // it's cut short to maxKeptSaveNameLength, and trailing dots and spaces
    // go. Starting with the number, it's never a reserved name like CON or NUL.
    std::string keptSaveFolderName (int number, const commands::SaveInfo& info);

    class ProjectFolder final : public commands::Storage
    {
    public:
        // When a save happens; the system clock, unless a test says otherwise.
        using Clock = std::function<std::chrono::system_clock::time_point()>;

        // `loaded` is the project as read from the folder, or an empty
        // project for a new folder: its clip files are the ones Hocket has
        // had in the project so far.
        ProjectFolder (std::filesystem::path folder, const project::Project& loaded, Clock clock = {});

        const std::filesystem::path& path() const { return folder; }

        // A save that changes no file, but has a name, names the version on
        // disk: only its save.json is written, and nothing is kept.
        commands::SaveResult save (const project::Project& project, const commands::Save& request, const commands::Author& author) override;

        std::vector<commands::KeptSave> keptSaves() override;
        project::LoadResult loadKeptSave (const commands::KeptSave& save) override;

    private:
        std::filesystem::path folder;
        Clock clock;

        // Every clip file Hocket has had in the project here, loaded or
        // saved, that may still be on disk: the only files a save may move
        // to unused/.
        std::set<std::string, std::less<>> clipFiles;

        // Copies the version on disk into a new kept save. nullopt with no
        // problem when there's no project on disk to keep.
        std::optional<commands::KeptSave> keepVersionOnDisk (std::string& problem);
    };

    // Saves an empty project (120 BPM, 4/4, no tracks) in `folder`, which
    // must be new or empty; a new folder is made, with any folders above it.
    // Returns what went wrong, or empty.
    std::string createProject (const std::filesystem::path& folder, const commands::Author& author);
}
