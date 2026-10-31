#include "core/Paths.h"

#include <fstream>
#include <sstream>
#include <type_traits>

namespace hocket
{
std::filesystem::path pathFromUtf8 (std::string_view utf8)
{
    // char8_t text is always read as UTF-8, whatever the platform's code page.
    return std::filesystem::path (std::u8string (utf8.begin(), utf8.end()));
}

namespace
{
    void appendUtf8 (std::string& out, char32_t c)
    {
        if (c < 0x80)
        {
            out += static_cast<char> (c);
        }
        else if (c < 0x800)
        {
            out += static_cast<char> (0xC0 | (c >> 6));
            out += static_cast<char> (0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char> (0xE0 | (c >> 12));
            out += static_cast<char> (0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char> (0xF0 | (c >> 18));
            out += static_cast<char> (0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char> (0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (c & 0x3F));
        }
    }

    bool isHighSurrogate (char32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
    bool isLowSurrogate (char32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
}

std::string utf8FromPath (const std::filesystem::path& path)
{
    if constexpr (std::is_same_v<std::filesystem::path::value_type, wchar_t>)
    {
        // Windows names are UTF-16 that needn't be valid, and the standard
        // library's conversion throws on an unpaired surrogate (MSVC's does).
        const auto& wide = path.native();
        std::string utf8;
        utf8.reserve (wide.size());

        for (std::size_t i = 0; i < wide.size(); ++i)
        {
            auto c = static_cast<char32_t> (wide[i]);

            if (isHighSurrogate (c) && i + 1 < wide.size() && isLowSurrogate (static_cast<char32_t> (wide[i + 1])))
                c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t> (wide[++i]) - 0xDC00);
            else if (isHighSurrogate (c) || isLowSurrogate (c))
                c = 0xFFFD; // the replacement character

            appendUtf8 (utf8, c);
        }

        return utf8;
    }
    else
    {
        // Elsewhere, names are bytes, which Hocket takes to be UTF-8.
        const auto utf8 = path.u8string();
        return { utf8.begin(), utf8.end() };
    }
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
