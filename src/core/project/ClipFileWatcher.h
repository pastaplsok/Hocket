#pragma once

// Notices when a project's clip files change on disk, for
// `hocket-headless --play --watch`, which sends each change to the bus as a
// write_clip.
//
// A change is reported only once the file has been left alone for the settle
// time, so an editor that saves in several steps, or writes a temporary file
// and renames it, is never caught half-way.
//
// It polls rather than using OS notifications: call poll() regularly, e.g.
// every 50 ms. Polling a few files is cheap and works the same everywhere.

#include "core/project/Project.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hocket::project
{
    class ClipFileWatcher
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr Clock::duration defaultSettleTime = std::chrono::milliseconds (150);

        struct Change
        {
            std::string clipId;
            std::string file;                // relative to the project folder, e.g. "clips/bass.notes"
            std::optional<std::string> text; // nullopt if the file is gone or can't be read
        };

        // Watches every clip file in the project. What's on disk now is the
        // starting point: only later changes are reported.
        ClipFileWatcher (const std::filesystem::path& folder, const Project& project, Clock::duration settleTime = defaultSettleTime);

        // Files that changed and have since settled, each reported once per change.
        std::vector<Change> poll (Clock::time_point now = Clock::now());

    private:
        struct Stamp
        {
            bool exists = false;
            std::filesystem::file_time_type modified {};
            std::uintmax_t size = 0;

            bool operator== (const Stamp&) const = default;
        };

        struct WatchedFile
        {
            std::string clipId;
            std::string file;
            std::filesystem::path path;
            Stamp lastSeen;
            std::optional<Clock::time_point> changedAt; // set while a change is settling
        };

        Clock::duration settleTime;
        std::vector<WatchedFile> files;

        static Stamp stampOf (const std::filesystem::path& path);
    };
}
