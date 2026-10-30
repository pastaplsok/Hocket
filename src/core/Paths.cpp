#include "core/Paths.h"

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
}
