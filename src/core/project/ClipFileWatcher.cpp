#include "core/project/ClipFileWatcher.h"

#include "core/Paths.h"

namespace hocket::project
{
ClipFileWatcher::ClipFileWatcher (const std::filesystem::path& folder, const Project& project, Clock::duration settle)
    : settleTime (settle)
{
    for (const auto& track : project.tracks)
    {
        for (const auto& clip : track.clips)
        {
            auto path = folder / pathFromUtf8 (clip.file);
            auto stamp = stampOf (path);
            files.push_back ({ clip.id, clip.file, std::move (path), stamp, {} });
        }
    }
}

std::vector<ClipFileWatcher::Change> ClipFileWatcher::poll (Clock::time_point now)
{
    std::vector<Change> changes;

    for (auto& file : files)
    {
        const auto stamp = stampOf (file.path);

        // Still being written, or changed again: wait for it to settle from now.
        if (stamp != file.lastSeen)
        {
            file.lastSeen = stamp;
            file.changedAt = now;
            continue;
        }

        if (file.changedAt && now - *file.changedAt >= settleTime)
        {
            file.changedAt.reset();
            changes.push_back ({ file.clipId, file.file, stamp.exists ? readTextFile (file.path) : std::nullopt });
        }
    }

    return changes;
}

ClipFileWatcher::Stamp ClipFileWatcher::stampOf (const std::filesystem::path& path)
{
    // Mid-rename, the file can vanish between these calls; it then counts as missing.
    std::error_code isFile, timeError, sizeError;
    Stamp stamp;

    if (! std::filesystem::is_regular_file (path, isFile))
        return stamp;

    stamp.modified = std::filesystem::last_write_time (path, timeError);
    stamp.size = std::filesystem::file_size (path, sizeError);

    if (timeError || sizeError)
        return {};

    stamp.exists = true;
    return stamp;
}
}
