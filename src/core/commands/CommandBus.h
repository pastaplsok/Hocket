#pragma once

// The command bus (SPEC §3.1): the only way to change a project. Every client
// (GUI, scripts, MCP) sends the same commands; each is validated before it
// applies, and each applied change is recorded in one linear undo history,
// tagged with its author.
//
// Not thread-safe: use it from the message thread only (SPEC §3.2).

#include "core/commands/Changes.h"
#include "core/commands/Commands.h"
#include "core/commands/Edits.h"
#include "core/project/Project.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hocket::commands
{
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

        // The entries in history() that changed this clip file's text, newest
        // first: its notes, or how they're written (the time signature, the
        // drum map it's written with).
        std::vector<HistoryEntry> changesTo (std::string_view file) const;

        std::optional<OpenTransaction> openTransaction() const;

        // Called with each change, after every command, undo, redo and
        // rollback, once all of its edits have applied, so a listener sees
        // the project whole. Returns an ID for removeListener. Listeners
        // mustn't add or remove listeners.
        int addListener (std::function<void (const Change&)> listener);
        void removeListener (int listenerId);

        // Where transport commands go. Pass nullptr to remove it; a transport that
        // goes away before the bus must do that first.
        void setTransport (Transport* transport);

        // Whether the transport is playing; nullopt when there's no transport.
        std::optional<bool> isPlaying() const;

    private:
        using Edit = edits::Edit;

        // Edits recorded together: a history entry, or an open transaction's current part.
        struct Edits
        {
            std::vector<Edit> forward;       // in the order they were applied
            std::vector<Edit> inverse;       // the same edits' inverses, in the same order
            edits::Files rewritten;          // the clip files whose text they changed (changesTo)

            bool empty() const { return forward.empty(); }
            void append (Edits&& more);
        };

        struct Entry
        {
            HistoryEntry info;
            Edits edits;
        };

        struct Transaction
        {
            Author author;
            std::string name;
            int part = 1; // > 1 once another author's command has split it
            Edits edits;  // the current part's
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
        edits::Plan plan (const Command&) const;

        // Applies a command's edits, and says what they did.
        Edits applyCommand (std::vector<Edit> forward);
        void applyForward (const Edits&);
        void applyInverse (const Edits&);

        void record (const Author& author, std::string description, Edits edits);
        void closeTransactionPart();
        void notify (const std::vector<Change>&);
    };
}
