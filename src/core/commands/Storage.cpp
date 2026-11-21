#include "core/commands/Storage.h"

#include <format>

namespace hocket::commands
{
std::string localTimeText (SaveTime time)
{
    // The time zone database can be missing (an old Windows, a stripped
    // system): then it's UTC, and says so.
    try
    {
        const auto local = std::chrono::current_zone()->to_local (time);
        return std::format ("{:%Y-%m-%d %H:%M}", std::chrono::floor<std::chrono::minutes> (local));
    }
    catch (const std::exception&)
    {
        return std::format ("{:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes> (time));
    }
}

std::string savedBy (const SaveInfo& info)
{
    if (! info.author.empty())
        return info.author;

    return info.discardedRecovery ? "discarded recovery" : info.changedOutside ? "changed outside Hocket" : "before Hocket";
}

std::string describe (const KeptSave& save)
{
    const auto by = ! save.info.author.empty()    ? "by " + save.info.author
                  : save.info.discardedRecovery ? std::string ("a discarded recovery copy")
                  : save.info.changedOutside    ? std::string ("changed outside Hocket")
                                                : std::string ("from before Hocket");
    auto text = "save #" + std::to_string (save.number) + ", " + by + ", " + localTimeText (save.info.time);

    if (! save.info.name.empty())
        text += ", \"" + save.info.name + "\"";

    return text;
}
}
