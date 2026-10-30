#pragma once

// Non-ASCII names for testing file handling (CLAUDE.md), and a temporary folder.

#include "core/Paths.h"

#include <filesystem>
#include <random>
#include <string>

namespace hocket::testing
{
    // Greek and Japanese are outside every Western ANSI code page, so text that
    // went through one comes out as '?'.
    inline const std::string unicodeName = "ünï ελλ テスト";

    // A new folder in the temp directory, deleted when the test ends.
    struct TemporaryFolder
    {
        explicit TemporaryFolder (const std::string& utf8Name)
            : path (std::filesystem::temp_directory_path() / pathFromUtf8 (utf8Name + " " + std::to_string (std::random_device {}())))
        {
            std::filesystem::create_directories (path);
        }

        ~TemporaryFolder()
        {
            std::error_code error;
            std::filesystem::remove_all (path, error);
        }

        TemporaryFolder (const TemporaryFolder&) = delete;
        TemporaryFolder& operator= (const TemporaryFolder&) = delete;

        std::filesystem::path path;
    };
}
