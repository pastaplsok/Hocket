#pragma once

// A file's size and modified time: how Hocket tells whether a project file
// changed on disk since it last read or wrote it (SPEC §4.5, §6.1).

#include <cstdint>
#include <filesystem>

namespace hocket::project
{
    struct FileStamp
    {
        bool exists = false;
        std::filesystem::file_time_type modified {};
        std::uintmax_t size = 0;

        bool operator== (const FileStamp&) const = default;

        // A file that isn't there, or can't be looked at, doesn't exist.
        static FileStamp of (const std::filesystem::path& path)
        {
            // Mid-rename, the file can vanish between these calls; it then counts as missing.
            std::error_code isFile, timeError, sizeError;
            FileStamp stamp;

            if (! std::filesystem::is_regular_file (path, isFile))
                return stamp;

            stamp.modified = std::filesystem::last_write_time (path, timeError);
            stamp.size = std::filesystem::file_size (path, sizeError);

            if (timeError || sizeError)
                return {};

            stamp.exists = true;
            return stamp;
        }
    };
}
