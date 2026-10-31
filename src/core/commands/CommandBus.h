#pragma once

// The command bus (SPEC §3.1): the only way to change a project. Every client
// (GUI, scripts, MCP) sends the same commands; each is validated before it
// applies, and each applied change is recorded in one linear undo history,
// tagged with its author.
//
// Not thread-safe: use it from the message thread only (SPEC §3.2).

#include "core/project/Project.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hocket::commands
{
    // Replaces a clip's notes with new .notes text (SPEC §6.2).
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

    // Carries out transport commands for the bus. The engine provides one; without
    // it, transport commands fail.
    class Transport
    {
    public:
        virtual ~Transport() = default;

        virtual Result play() = 0;
        virtual Result stop() = 0;
        virtual bool isPlaying() const = 0;
    };

    struct HistoryEntry
    {
        int id = 0;              // stable: "#7"
        Author author;
        std::string description; // "wrote clip clp-bass (14 notes)"

        bool operator== (const HistoryEntry&) const = default;
    };

    struct OpenTransaction
    {
        Author author;
        std::string name;      // "wrote bridge"
        int part = 1;          // > 1 once another author's command has split it
        bool hasEdits = false; // in the current part
    };

    // What changed in the project, so the engine can update just that (SPEC §3.3).
    struct ClipContentChanged
    {
        std::string clipId;
    };

    using Change = std::variant<ClipContentChanged>;

    // Whose changes an undo may take back. The GUI undoes anything; AI clients
    // undo only their own changes unless they pass force (SPEC §3.1).
    enum class UndoScope
    {
        anyAuthor,
        callersOwn
    };

    class CommandBus
    {
    public:
        explicit CommandBus (project::Project project);

        const project::Project& project() const { return currentProject; }

        Result execute (const Command& command, const Author& author);

        Result beginTransaction (const Author& author, const std::string& name);
        Result commitTransaction (const Author& author);
        Result rollbackTransaction (const Author& author);

        // Closes the author's open transaction when its client can't: it
        // disconnected, or stopped sending requests (SPEC §3.1). The current
        // part's edits become an entry marked "(interrupted)" that keeps the
        // transaction's author, so it can be undone like any other; a part with
        // no edits just closes. The message says what became of it, starting
        // with "transaction \"<name>\" was closed".
        Result interruptTransaction (const Author& author);

        Result undo (const Author& author, UndoScope scope);
        Result redo (const Author& author);

        // Newest first. An open transaction's edits aren't listed until they're committed.
        std::vector<HistoryEntry> history() const;

        // The entries in history() that changed this clip, newest first.
        std::vector<HistoryEntry> changesTo (std::string_view clipId) const;

        std::optional<OpenTransaction> openTransaction() const;

        // Called after every change to the project, including undo, redo and rollback.
        // Returns an ID for removeListener. Listeners mustn't add or remove listeners.
        int addListener (std::function<void (const Change&)> listener);
        void removeListener (int listenerId);

        // Where transport commands go. Pass nullptr to remove it; a transport that
        // goes away before the bus must do that first.
        void setTransport (Transport* transport);

        // Whether the transport is playing; nullopt when there's no transport.
        std::optional<bool> isPlaying() const;

    private:
        struct SetClipContent
        {
            std::string clipId;
            notes::Clip content;
        };

        using Edit = std::variant<SetClipContent>;

        struct Entry
        {
            HistoryEntry info;
            std::vector<Edit> forward; // in the order they were applied
            std::vector<Edit> inverse; // the same edits' inverses, in the same order
        };

        struct Transaction
        {
            Author author;
            std::string name;
            int part = 1;              // > 1 once another author's command has split it
            std::vector<Edit> forward; // the current part's edits
            std::vector<Edit> inverse;
        };

        project::Project currentProject;
        std::vector<Entry> done;
        std::vector<Entry> undone;
        std::optional<Transaction> transaction;
        int nextEntryId = 1;
        std::vector<std::pair<int, std::function<void (const Change&)>>> listeners;
        int nextListenerId = 1;
        Transport* transport = nullptr;

        Result runTransportCommand (const Command&);
        Result validate (const WriteClip&, std::vector<Edit>& edits, std::string& description) const;
        Edit apply (const Edit&);
        void record (const Author& author, std::string description, std::vector<Edit> forward, std::vector<Edit> inverse);
        void closeTransactionPart();
        void notify (const Change&);
    };
}
