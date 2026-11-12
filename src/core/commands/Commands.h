#pragma once

// The commands clients send to the command bus (SPEC §3.1), who sends them,
// and what comes back.
//
// Values are written as project.json and .notes write them: positions like
// "9.1", volumes like "-6dB", so the bus validates them with the format's own
// parsers, and its errors are the format's.

#include <string>
#include <variant>

namespace hocket::commands
{
    // Replaces a clip's notes with new .notes text (SPEC §6.2). The notes are
    // its file's, so clips linked to it change too. A new @length resizes them all.
    struct WriteClip
    {
        std::string clipId;
        std::string notes;
    };

    // Transport commands (SPEC §3.1). They don't change the project, so they're
    // never validated against it, never enter the history, and don't split an
    // open transaction.
    struct Play
    {
    };

    struct Stop
    {
    };

    using Command = std::variant<WriteClip, Play, Stop>;

    // Who sends a command (SPEC §3.1). Two authors are the same when their names
    // and sessions match, so two Claude sessions are different authors, and a
    // client that reconnects with its session ID is still the author of its own
    // changes. The label only tells same-named authors apart in messages.
    struct Author
    {
        std::string name;    // "Lee", "Claude", "script:walking-bass"
        std::string session; // the client's session ID; empty for the person at the DAW
        std::string label;   // optional, chosen by the client, e.g. "drums"

        bool operator== (const Author& other) const { return name == other.name && session == other.session; }
    };

    // "Claude", or "Claude (drums)" with a label.
    std::string displayName (const Author& author);

    // How a message to `reader` names `author`: "you" for the reader; otherwise
    // the display name, plus " in another session" when it alone would look
    // like the reader's own.
    std::string nameFor (const Author& author, const Author& reader);

    struct Result
    {
        bool ok = false;
        std::string message; // on success, what happened; on failure, what's wrong and how to fix it
    };
}
