#pragma once

// The recovery copy on disk (SPEC §4.5): the session's project, written
// outside its folder, so a crash loses nothing that wasn't saved.
//
// Each project has a place of its own in the recovery folder
// (%LOCALAPPDATA%\Hocket\Recovery on Windows), named for its folder and a hash
// of its full path: "my song 3f2a9c81d4e6b7a0". The copy in it is a project
// folder of its own, numbered: "7\project.json", "7\clips\bass.notes", and
// "7\.hocket\recovery.json", which names the project's folder and says when the
// copy was written. So it loads, and --check reads it, like any project.
//
// A crash or power loss part-way through writing a copy leaves the copy before
// it whole and usable, never part of the new one:
//   1. the new copy is written whole into "writing 8\", each file on the disk
//      (platform::writeFileToDisk) before the next is begun;
//   2. then renamed "8" in one step, which is on the disk before it returns
//      (platform::moveFile). NTFS journals its changes to folders in order,
//      so once the rename is on the disk, so are the files it names;
//   3. only then is "7" taken away: renamed "removing 7", then deleted.
// Reading takes the highest-numbered copy, and never a "writing" or "removing"
// folder, which the next write clears away. Removing a copy renames it first
// too, so a crash part-way leaves it whole or gone, never half there.
//
// Message thread only.

#include "core/commands/Storage.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace hocket::storage
{
    // In each copy: which project it's of, and when it was written.
    inline constexpr std::string_view recoveryInfoFile = ".hocket/recovery.json";

    // In the project's own folder: an ID of its own, 32 hex digits, made when
    // its first recovery copy is written. It moves with the folder, so a copy
    // is found again after the folder is moved or renamed. Kept saves don't
    // copy it.
    inline constexpr std::string_view projectIdFile = ".hocket/id";

    // How long the project folder's name may be in its place's name, in
    // UTF-16 code units, so a copy's files' paths stay well inside Windows' 260.
    inline constexpr std::size_t maxRecoveryNameLength = 40;

    // %LOCALAPPDATA%\Hocket\Recovery on Windows; nullopt if the OS doesn't
    // say where app data goes.
    std::optional<std::filesystem::path> defaultRecoveryFolder();

    class RecoveryCopy
    {
    public:
        using Clock = std::function<std::chrono::system_clock::time_point()>;

        // `recoveryFolder` holds every project's recovery copies; this one's
        // place in it is named for `projectFolder`'s full path, the same
        // however that's written (relative, in another case, with "..").
        // The clock says when a copy is written; the system's, unless a test
        // says otherwise.
        RecoveryCopy (std::filesystem::path recoveryFolder, const std::filesystem::path& projectFolder, Clock clock = {});

        // This project's place in the recovery folder.
        const std::filesystem::path& place() const { return placePath; }

        // The project folder's full path, as each copy's recovery.json names it.
        const std::string& project() const { return projectPath; }

        struct Found
        {
            std::filesystem::path folder; // the copy: a project folder
            commands::SaveTime written;   // when, to the second
        };

        // The newest whole copy of this project, if there is one.
        std::optional<Found> find() const;

        // Copies of this project kept for its folder's old path: the folder
        // was moved or renamed since a session left them. Found by the ID in
        // the project's .hocket/id, which they name; not when the old path
        // still holds a project with that ID (a copied folder: the copies
        // are the other's). Only when this folder has no copy of its own.
        // Their place is renamed to this folder's, so from then on they're
        // its own; returns the old folder's path, for a message.
        std::optional<std::string> adoptMoved();

        // Writes these files (relative paths to their text, as project::write
        // gives them) as the new copy, then takes the one before away. Returns
        // what went wrong, or empty; when it fails, the copy before is still
        // the one find() finds.
        std::string write (const std::map<std::string, std::string>& files);

        // Takes this project's copies away. Returns what went wrong, or empty.
        std::string remove();

        // For tests of a crash part-way: each change a write or remove makes
        // to the disk, in order.
        struct Step
        {
            enum class Kind
            {
                clearLeftover,   // deleting a "writing" or "removing" folder a crash left
                writeFile,       // writing one of the new copy's files, `path`
                commit,          // renaming the new copy from "writing <n>" to `path`, "<n>"
                takeAway,        // renaming a copy, `path`, to "removing <n>"
                deleteTakenAway  // deleting "removing <n>", `path`
            };

            Kind kind;
            std::filesystem::path path;
        };

        // Called before each step; returning false stops the write or remove
        // there, as a crash would: nothing after it happens, and nothing is
        // cleaned up. For tests only.
        void setStepHook (std::function<bool (const Step&)> hook) { stepHook = std::move (hook); }

    private:
        std::filesystem::path placePath;
        std::string projectPath;
        std::filesystem::path projectFolderPath;
        std::string projectId; // its .hocket/id; empty until its first copy makes one

        // Whether a copy naming this project path and ID is this project's.
        bool belongs (const std::string& project, const std::string& id) const;

        // Makes the project's ID and writes it to its .hocket/id; empty if it can't.
        std::string makeProjectId() const;
        Clock clock;
        std::function<bool (const Step&)> stepHook;

        bool step (Step::Kind kind, const std::filesystem::path& path) const;

        // The numbers of the copies in the place, highest first, whether or
        // not they read.
        std::vector<int> copyNumbers() const;

        // Deletes what a crash left part-way. False if the step hook stopped it.
        bool clearLeftovers() const;

        // Renames a copy "removing <n>", then deletes it. Returns what went
        // wrong, or empty; "stopped" if the step hook stopped it.
        std::string takeAway (int number) const;

        // The project the newest copy that reads is of, if it isn't this one.
        std::optional<std::string> otherProject() const;
    };
}
