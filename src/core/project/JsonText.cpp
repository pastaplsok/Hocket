#include "core/project/JsonText.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace hocket::project
{
LineIndex::LineIndex (std::string_view text)
{
    lineStarts.push_back (0);

    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
            lineStarts.push_back (i + 1);
}

TextPosition LineIndex::positionOf (std::size_t offset) const
{
    const auto next = std::ranges::upper_bound (lineStarts, offset);
    const auto line = static_cast<std::size_t> (next - lineStarts.begin());

    return { static_cast<int> (line), static_cast<int> (offset - lineStarts[line - 1]) + 1 };
}

std::vector<KeyAt> keysOfObject (std::string_view text, std::size_t objectStart)
{
    std::vector<KeyAt> keys;
    int depth = 0; // how deep inside this object's values we are
    bool expectingKey = true;

    for (auto i = objectStart + 1; i < text.size(); ++i)
    {
        const auto c = text[i];

        if (c == '"')
        {
            const auto start = i;

            for (++i; i < text.size() && text[i] != '"'; ++i)
                if (text[i] == '\\')
                    ++i;

            if (depth == 0 && expectingKey)
            {
                const auto raw = text.substr (start, i - start + 1);
                auto key = raw.find ('\\') == std::string_view::npos
                             ? std::string (raw.substr (1, raw.size() - 2))
                             : nlohmann::json::parse (raw).get<std::string>();

                keys.push_back ({ std::move (key), start });
                expectingKey = false;
            }
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
        }
        else if (c == '}' || c == ']')
        {
            if (depth == 0)
                break;

            --depth;
        }
        else if (c == ',' && depth == 0)
        {
            expectingKey = true;
        }
    }

    return keys;
}

std::size_t stringStart (std::string_view text, std::size_t end)
{
    for (auto i = end - 1; i-- > 0;)
    {
        if (text[i] != '"')
            continue;

        std::size_t backslashes = 0;

        for (auto j = i; j > 0 && text[j - 1] == '\\'; --j)
            ++backslashes;

        if (backslashes % 2 == 0)
            return i;
    }

    return 0;
}
}
