// What the command plans share.

#include "core/commands/Planning.h"

namespace hocket::commands::planning
{
edits::Plan refused (std::string message)
{
    return { { false, std::move (message) }, {}, {}, {} };
}

edits::Plan unchanged (std::string message)
{
    return { { true, std::move (message) }, {}, {}, {} };
}

std::string singleQuoted (std::string_view text)
{
    return "'" + std::string (text) + "'";
}

std::string doubleQuoted (std::string_view text)
{
    return "\"" + std::string (text) + "\"";
}

std::string barsText (int bars)
{
    return std::to_string (bars) + (bars == 1 ? "bar" : "bars");
}

std::string countText (std::size_t count, std::string_view noun)
{
    return std::to_string (count) + " " + std::string (noun) + (count == 1 ? "" : "s");
}

std::string listText (const std::vector<std::string>& items)
{
    std::string text;

    for (std::size_t i = 0; i < items.size(); ++i)
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];

    return text;
}
}
