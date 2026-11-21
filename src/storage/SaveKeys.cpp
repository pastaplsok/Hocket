#include "storage/SaveKeys.h"

namespace hocket::storage
{
namespace
{
    // "saved: wrote project.json" becomes "Saved: wrote project.json": a line of its own.
    std::string sentence (std::string text)
    {
        if (! text.empty() && text.front() >= 'a' && text.front() <= 'z')
            text.front() = static_cast<char> (text.front() - 'a' + 'A');

        return text;
    }
}

SaveKeys::SaveKeys (commands::CommandBus& commandBus, commands::Author localPerson)
    : bus (commandBus), person (std::move (localPerson))
{
}

std::optional<std::string> SaveKeys::press (char32_t key)
{
    if (key == U's' || key == U'S')
        return save (false);

    if (key == U'o' || key == U'O')
    {
        if (! offer)
            return std::string ("O saves over files changed outside Hocket, only straight after S was refused for them. S saves.");

        if (*offer != bus.revision())
        {
            offer.reset();
            return std::string ("The project changed after that save was refused, so O doesn't save over the files changed outside Hocket. "
                                "Press S to try again.");
        }

        return save (true);
    }

    if (offer)
    {
        offer.reset();
        return std::string ("Left the files changed outside Hocket as they are; nothing was saved.");
    }

    return {};
}

std::string SaveKeys::save (bool overwrite)
{
    offer.reset();
    commands::Save request;
    request.overwrite = overwrite;
    const auto result = bus.save (request, person);

    if (result.ok || result.outsideChanges.empty())
        return sentence (result.message);

    // Refused for changes made outside Hocket: said for the person at the
    // console, who can save over them with O.
    offer = bus.revision();
    const auto one = result.outsideChanges.size() == 1;
    std::string text = std::string ("Didn't save, so as not to overwrite ") + (one ? "a file" : "files") + " changed since Hocket last read or wrote "
                     + (one ? "it" : "them") + ": ";

    for (std::size_t i = 0; i < result.outsideChanges.size(); ++i)
        text += (i == 0 ? "" : "; ") + result.outsideChanges[i];

    return text + ". Nothing on disk changed.\nPress O to save over " + (one ? "it" : "them") + " anyway: what's on disk is kept as a kept save first, "
         + "so it can be got back. Any other key leaves " + (one ? "it as it is." : "them as they are.");
}
}
