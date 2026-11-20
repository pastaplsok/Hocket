#pragma once

// Notices when a project's files change outside Hocket, for
// `hocket-headless --play --watch` (SPEC §4.5, §6.1): project.json and each
// clip file the session's project uses, against what Hocket last read or
// wrote of them (ProjectFolder). So Hocket's own saves are never changes: a
// save leaves the files as Hocket knows them.
//
// A change is reported once the file has been left alone for the settle
// time, so an editor that saves in several steps, or writes a temporary file
// and renames it, is never caught half-way. Each change is reported once:
// one the session didn't take in (refused, say) isn't reported again until
// the file changes again.
//
// It polls rather than using OS notifications: call poll() regularly, e.g.
// every 50 ms, on the thread that saves. Polling a few files is cheap and
// works the same everywhere.

#include "storage/ProjectFolder.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hocket::storage
{
    class ProjectWatcher
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr Clock::duration defaultSettleTime = std::chrono::milliseconds (150);

        // How long to keep trying a settled file that can't be read (an editor
        // may still hold it) before reporting it as unreadable.
        static constexpr Clock::duration readRetryTime = std::chrono::seconds (2);

        struct Change
        {
            std::string file;                // "project.json", or a clip file: "clips/bass.notes"
            std::optional<std::string> text; // nullopt if the file is gone, or still can't be read after readRetryTime
            project::FileStamp stamp;        // the file's when it was read: what the folder notes once the change is taken in
        };

        explicit ProjectWatcher (const ProjectFolder& folder, Clock::duration settleTime = defaultSettleTime);

        // The files of `project` (project.json, then its clip files) that
        // differ on disk from what the folder last read or wrote, once
        // settled. A file the folder never had (a new clip's, until it's
        // saved) isn't watched.
        std::vector<Change> poll (const project::Project& project, Clock::time_point now = Clock::now());

    private:
        struct Watched
        {
            project::FileStamp seen;                   // on disk at the last poll
            std::optional<Clock::time_point> changedAt; // while it differs from the folder's: since when it's been `seen`
            std::optional<project::FileStamp> reported; // the last change reported, until the file matches the folder's again
        };

        const ProjectFolder& folder;
        Clock::duration settleTime;
        std::map<std::string, Watched, std::less<>> files;
    };
}
