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
    };

    struct HistoryEntry
    {
        int id = 0;              // stable: "#7"
        std::string author;      // "Lee", "Claude", "script:walking-bass", ...
        std::string description; // "wrote clip clp-bass (14 notes)"

        bool operator== (const HistoryEntry&) const = default;
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

        Result execute (const Command& command, const std::string& author);

        Result beginTransaction (const std::string& author, const std::string& name);
        Result commitTransaction (const std::string& author);
        Result rollbackTransaction (const std::string& author);

        Result undo (const std::string& author, UndoScope scope);
        Result redo (const std::string& author);

        // Newest first. An open transaction's edits aren't listed until they're committed.
        std::vector<HistoryEntry> history() const;

        // Called after every change to the project, including undo, redo and rollback.
        // Returns an ID for removeListener. Listeners mustn't add or remove listeners.
        int addListener (std::function<void (const Change&)> listener);
        void removeListener (int listenerId);

        // Where transport commands go. Pass nullptr to remove it; a transport that
        // goes away before the bus must do that first.
        void setTransport (Transport* transport);

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
            std::string author;
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
        void record (const std::string& author, std::string description, std::vector<Edit> forward, std::vector<Edit> inverse);
        void closeTransactionPart();
        void notify (const Change&);
    };
}
