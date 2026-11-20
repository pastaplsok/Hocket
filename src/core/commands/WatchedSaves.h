#pragma once

// hocket-headless --play --watch's way into the bus for clip files (SPEC
// §6.1): each saved clip file goes in as the person's write_clip, to the
// first clip using it (linked clips share it). A change made some other way
// since the file was last loaded or saved, like an AI client's write_clip,
// isn't in the file, so the file's save replaces it. That's undoable, but
// easy to miss, so the save comes with a warning naming what it replaced.
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
        // The clip files' notes as they are now are taken to be what the
        // files hold, as when the project has just been loaded.
        WatchedSaves (CommandBus& bus, Author person);

        struct Outcome
        {
            Result result;                      // the write_clip's
            std::optional<std::string> warning; // when the save replaced a change the file didn't have
        };

        // The text of a clip file ("clips/bass.notes"), just saved.
        Outcome save (const std::string& file, const std::string& text);

        // The notes' canonical text when they last matched the file, as
        // Hocket last saved or read it (storage::ProjectFolder knows).
        void matched (const std::string& file, std::string text) { matchedFile[file] = std::move (text); }

    private:
        CommandBus& bus;
        Author person;
        std::map<std::string, std::string, std::less<>> matchedFile; // canonical text of each file's notes when they last matched it

        std::string warningFor (const std::string& file) const;
    };
}
