#include "storage/ProjectWatcher.h"

#include "core/Paths.h"

namespace hocket::storage
{
ProjectWatcher::ProjectWatcher (const ProjectFolder& projectFolder, Clock::duration settle) : folder (projectFolder), settleTime (settle)
{
}

std::vector<ProjectWatcher::Change> ProjectWatcher::poll (const project::Project& project, Clock::time_point now)
{
    // project.json first: a change to it is read with the clip files as they are.
    std::vector<std::string> names { "project.json" };

    for (const auto& entry : project.notesFiles)
        names.push_back (entry.first);

    std::erase_if (files, [&names] (const auto& file) { return std::ranges::find (names, file.first) == names.end(); });

    std::vector<Change> changes;

    for (const auto& name : names)
    {
        const auto known = folder.knownStamp (name);

        if (! known)
        {
            files.erase (name);
            continue;
        }

        const auto path = folder.path() / pathFromUtf8 (name);
        const auto stamp = project::FileStamp::of (path);
        auto [found, added] = files.try_emplace (name);
        auto& file = found->second;

        // Still being written, or changed again: wait for it to settle from now.
        if (added || stamp != file.seen)
        {
            file.seen = stamp;
            file.changedAt = now;

            if (! added || stamp != *known)
                continue;
        }

        // As Hocket last read or wrote it: Hocket's own save, or put back.
        if (stamp == *known)
        {
            file.changedAt.reset();
            file.reported.reset();
            continue;
        }

        if (file.reported == stamp)
            continue;

        if (! file.changedAt)
            file.changedAt = now;

        if (now - *file.changedAt < settleTime)
            continue;

        auto text = stamp.exists ? readTextFile (path) : std::nullopt;

        // An editor may still be holding the file: try again on the next poll, for a while.
        if (! text && stamp.exists && now - *file.changedAt < settleTime + readRetryTime)
            continue;

        file.changedAt.reset();
        file.reported = stamp;
        changes.push_back ({ name, std::move (text), stamp });
    }

    return changes;
}
}
