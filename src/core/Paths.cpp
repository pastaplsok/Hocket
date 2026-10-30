#include "core/Paths.h"

#include <fstream>
#include <sstream>

namespace hocket
{
std::filesystem::path pathFromUtf8 (std::string_view utf8)
{
    // char8_t text is always read as UTF-8, whatever the platform's code page.
    return std::filesystem::path (std::u8string (utf8.begin(), utf8.end()));
}

std::string utf8FromPath (const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return { utf8.begin(), utf8.end() };
}

std::optional<std::string> readTextFile (const std::filesystem::path& path)
{
    std::error_code error;

    if (! std::filesystem::is_regular_file (path, error))
        return {};

    std::ifstream in (path, std::ios::binary);

    if (! in)
        return {};

    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}
}
