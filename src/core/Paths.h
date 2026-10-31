#pragma once

// Text is UTF-8 everywhere in Hocket; paths are std::filesystem::path. These are
// the only conversions between the two (see CLAUDE.md).
//
// Never build a path straight from a std::string or char*, and never call
// path::string(): on Windows both go through the ANSI code page, which can't
// represent most non-ASCII folder names.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hocket
{
    // The text must be valid UTF-8.
    std::filesystem::path pathFromUtf8 (std::string_view utf8);

    // Never throws: on Windows, an unpaired UTF-16 surrogate in a name (which
    // NTFS allows) comes out as U+FFFD.
    std::string utf8FromPath (const std::filesystem::path& path);

    // A file's bytes, unchanged, or nullopt if it isn't a regular file or can't be read.
    std::optional<std::string> readTextFile (const std::filesystem::path& path);
}
