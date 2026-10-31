#pragma once

// hocket-headless --play --watch's way into the bus (SPEC §6.1): each saved
// clip file goes in as the person's write_clip. Milestone 0 never writes clip
// files, so a change made some other way, like an AI client's write_clip, isn't
// in the file, and the next save of that file replaces it. That's undoable,
// but easy to miss, so the save comes with a warning naming what it replaced.
//
// Message thread only, like the bus.

#include "core/commands/CommandBus.h"

#include <map>
#include <optional>
#include <string>

namespace hocket::commands
{
    class WatchedSaves
    {
    public:
        // The clips as they are now are taken to be what the files hold, as
        // when the project has just been loaded.
        WatchedSaves (CommandBus& bus, Author person);

        struct Outcome
        {
            Result result;                      // the write_clip's
            std::optional<std::string> warning; // when the save replaced a change the file didn't have
        };

        // The text of a clip's file, just saved.
        Outcome save (const std::string& clipId, const std::string& text);

    private:
        CommandBus& bus;
        Author person;
        std::map<std::string, std::string, std::less<>> matchedFile; // canonical text of each clip when it last matched its file

        std::string warningFor (const std::string& clipId) const;
    };
}
