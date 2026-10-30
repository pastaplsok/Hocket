#pragma once

// Text is UTF-8 everywhere in Hocket; paths are std::filesystem::path. These are
// the only conversions between the two (see CLAUDE.md).
//
// Never build a path straight from a std::string or char*, and never call
// path::string(): on Windows both go through the ANSI code page, which can't
// represent most non-ASCII folder names.

#include <filesystem>
#include <string>
#include <string_view>

namespace hocket
{
    // The text must be valid UTF-8.
    std::filesystem::path pathFromUtf8 (std::string_view utf8);

    std::string utf8FromPath (const std::filesystem::path& path);
}
