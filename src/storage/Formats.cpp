#include "storage/Formats.h"

#include <charconv>
#include <chrono>
#include <format>

namespace hocket::storage
{
namespace
{
    // The code point starting at text[i], moving i past it; nullopt for an
    // invalid sequence, moving i one byte on.
    std::optional<char32_t> nextCodePoint (std::string_view text, std::size_t& i)
    {
        const auto lead = static_cast<unsigned char> (text[i++]);

        if (lead < 0x80)
            return lead;

        const auto extra = lead >= 0xC2 && lead <= 0xDF ? 1 : lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;

        if (extra == 0 || i + static_cast<std::size_t> (extra) > text.size())
            return {};

        char32_t c = lead & (extra == 1 ? 0x1Fu : extra == 2 ? 0x0Fu : 0x07u);

        for (int k = 0; k < extra; ++k)
        {
            const auto next = static_cast<unsigned char> (text[i + static_cast<std::size_t> (k)]);

            if ((next & 0xC0) != 0x80)
                return {};

            c = (c << 6) | (next & 0x3Fu);
        }

        const char32_t smallest = extra == 1 ? 0x80 : extra == 2 ? 0x800 : 0x10000;

        if (c < smallest || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            return {};

        i += static_cast<std::size_t> (extra);
        return c;
    }

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
}

std::string windowsSafeName (std::string_view text, std::size_t maxUnits)
{
    constexpr std::string_view refused = "<>:\"/\\|?*";
    std::string name;
    std::size_t units = 0;

    for (std::size_t i = 0; i < text.size();)
    {
        auto c = nextCodePoint (text, i);

        if (! c)
            continue;

        if (*c < 0x20 || (*c >= 0x7F && *c < 0xA0) || (*c < 0x80 && refused.find (static_cast<char> (*c)) != std::string_view::npos))
            c = U' ';

        if (*c == U' ' && (name.empty() || name.back() == ' '))
            continue;

        const std::size_t size = *c >= 0x10000 ? 2 : 1;

        if (units + size > maxUnits)
            break;

        units += size;
        appendUtf8 (name, *c);
    }

    while (! name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();

    return name;
}

std::string utcTimeText (commands::SaveTime time)
{
    return std::format ("{:%Y-%m-%dT%H:%M:%SZ}", time);
}

std::optional<commands::SaveTime> parseUtcTime (std::string_view text)
{
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')
        return {};

    const auto number = [text] (std::size_t at, std::size_t length) -> std::optional<int>
    {
        int value = 0;
        const auto* end = text.data() + at + length;
        const auto [next, error] = std::from_chars (text.data() + at, end, value);
        return error == std::errc() && next == end ? std::optional<int> (value) : std::nullopt;
    };

    const auto year = number (0, 4), month = number (5, 2), day = number (8, 2);
    const auto hour = number (11, 2), minute = number (14, 2), second = number (17, 2);

    if (! year || ! month || ! day || ! hour || ! minute || ! second || *hour > 23 || *minute > 59 || *second > 59)
        return {};

    const std::chrono::year_month_day date { std::chrono::year (*year), std::chrono::month (static_cast<unsigned> (*month)),
                                             std::chrono::day (static_cast<unsigned> (*day)) };

    if (! date.ok())
        return {};

    return std::chrono::sys_days (date) + std::chrono::hours (*hour) + std::chrono::minutes (*minute) + std::chrono::seconds (*second);
}
}
