#pragma once

// `hocket-headless --play --watch` (SPEC §4.5, §6.1): files of the project
// saved outside Hocket go into the session as the person's commands, checked
// and undoable like any other.
//
// - project.json: one command replacing the project (ReplaceProject), read
//   with each clip file as the session has it, unsaved changes included, or
//   as it is on disk if it changed outside Hocket too, or is new. With a
//   warning when it replaces changes to the project that weren't saved.
// - A clip file: a write_clip to the first clip using it (WatchedSaves),
//   with a warning when it replaces changes the file didn't have.
//
// Once a change is taken in, the folder notes the file as read, so the next
// save doesn't count it as changed outside Hocket. One that's refused isn't,
// so the next save refuses to overwrite it, unless asked to.
//
// Message thread only, like the bus.

#include "core/commands/WatchedSaves.h"
#include "storage/ProjectWatcher.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hocket::storage
{
    class WatchedFolder
    {
    public:
        WatchedFolder (commands::CommandBus& bus, ProjectFolder& folder, commands::Author person,
                       ProjectWatcher::Clock::duration settleTime = ProjectWatcher::defaultSettleTime);

        struct Report
        {
            std::string file;                   // "project.json", "clips/bass.notes"
            bool ok = false;                    // taken in (or it changed nothing); otherwise the session carries on as it was
            std::string message;                // the command's result, or why it couldn't be taken in
            std::optional<std::string> warning; // a save that replaced changes the file didn't have
        };

        // The changes that have settled since the last poll, taken in.
        std::vector<Report> poll (ProjectWatcher::Clock::time_point now = ProjectWatcher::Clock::now());

    private:
        commands::CommandBus& bus;
        ProjectFolder& folder;
        commands::Author person;
        ProjectWatcher::Clock::duration settleTime;
        ProjectWatcher watcher;
        commands::WatchedSaves saves;

        // A project.json that couldn't be taken in: read again once a clip
        // file it names changes and settles (one saved after it, say), while
        // project.json stays as it was. A new save of project.json is the
        // watcher's to report.
        struct Refused
        {
            project::FileStamp stamp;                         // project.json's
            std::map<std::string, project::FileStamp> files;   // the clip files it named, as they were when it was read
            std::optional<ProjectWatcher::Clock::time_point> changedAt;
        };

        std::optional<Refused> refused;

        Report takeProject (const ProjectWatcher::Change& change);
        Report takeClip (const ProjectWatcher::Change& change);

        // project.json's save replaced changes to the project that weren't
        // saved: which, and the entry to undo to get them back.
        std::string replacedWarning() const;
    };
}
