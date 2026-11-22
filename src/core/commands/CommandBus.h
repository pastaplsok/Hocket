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
#include "core/commands/Storage.h"
#include "core/commands/Transport.h"
#include "core/kits/KitCatalog.h"
#include "core/project/Project.h"

#include <cstdint>
#include <limits>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hocket::commands
{
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

        // Carries out several commands as one history entry, for a client's
        // single request that changes several things (SPEC §6.4: set_track,
        // set_song). Each is planned against the project as the ones before
        // it leave it, and all apply or none does. The entry's description is
        // each changing command's, joined with "; "; commands that change
        // nothing are left out, and if none changes anything, nothing is
        // added. Transport commands can't be among them.
        Result executeAsOne (const std::vector<Command>& commands, const Author& author);

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

        // With callersOwn, it refuses when someone else did the undo it would
        // take back, as undo does for their changes (SPEC §3.1).
        Result redo (const Author& author, UndoScope scope = UndoScope::anyAuthor);

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

        Transport* currentTransport() const { return transport; }

        // The output's level (SPEC §6.4), restarted each time a transport play
        // starts. Without one, there's no level to give. It must outlive the
        // bus, or be removed with nullptr first.
        void setOutputMeter (OutputMeter* meter) { outputMeter = meter; }
        const OutputMeter* meter() const { return outputMeter; }

        // The loop region, looping, and the last seek's position: the
        // session's, changed only by transport commands (SPEC §3.1).
        const TransportSettings& transportSettings() const { return transportState; }

        // The installed drum kits (SPEC §8.4), which assigning a kit checks
        // against, and the overview describes. Without one, no kit is
        // installed. It must outlive the bus, or be removed with nullptr first.
        void setKitCatalog (kits::KitCatalog* catalog) { kitCatalog = catalog; }
        kits::KitCatalog* kits() const { return kitCatalog; }

        //==============================================================================
        // Saving (SPEC §4.5). The project the bus starts with counts as saved:
        // it's what was loaded.

        // Where save() writes. Without one, saving fails. It must outlive
        // the bus, or be removed with nullptr first.
        void setStorage (Storage* newStorage) { storage = newStorage; }

        // Any client can save, and it's not a history entry: like transport,
        // it changes nothing in the project, so it doesn't split an open
        // transaction or end a gesture. An open transaction's edits so far
        // are saved, since they're in the project. The version it replaces
        // is kept (SPEC §4.5).
        SaveResult save (const Save& request, const Author& author);

        // The versions saves have replaced, oldest first, which Revert
        // brings back; none without storage.
        std::vector<KeptSave> keptSaves() const;

        // Whether the project differs from the last save, as far as the
        // history can tell: undoing (or redoing) back to the saved state
        // counts as saved. A change that's then put back by hand, with no
        // undo, still counts as unsaved.
        bool hasUnsavedChanges() const { return revision() != savedRevision; }

        // The project the bus started with isn't what's on disk: it came from
        // a recovery copy, since the folder's files didn't load. Then no state
        // the history reaches counts as saved until a save.
        void markUnsaved() { savedRevision = std::numeric_limits<Revision>::max(); }

        // Each state of the project the history can reach has a revision
        // number, never reused: the newest entry's (0 when there's none), or
        // an open transaction's while it holds edits. Entries below the
        // newest can't change while it's there, so its number stands for the
        // whole state, and undo and redo go back to earlier numbers. So a
        // new number means the project changed, and the recovery copy may
        // need writing again (SPEC §4.5).
        using Revision = std::uint64_t;
        Revision revision() const;

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
            Revision revision = 0; // the state after its edits; a new one each time a gesture merges into it
            Author undoneBy;       // while it's in the undone list: who undid it
        };

        // The gesture whose next step can still merge into the newest entry (SPEC §3.1).
        struct Gesture
        {
            std::string id; // the client's
            Author author;
            std::string setting; // edits::Plan::setting
            int entryId = 0;
        };

        struct Transaction
        {
            Author author;
            std::string name;
            int part = 1; // > 1 once another author's command has split it
            Edits edits;  // the current part's
            Revision revision = 0; // the state after the current part's edits; the entry they become keeps it
        };

        project::Project currentProject;
        std::vector<Entry> done;
        std::vector<Entry> undone;
        std::optional<Transaction> transaction;
        std::optional<Gesture> gesture;
        int nextEntryId = 1;
        std::vector<std::pair<int, std::function<void (const Change&)>>> listeners;
        int nextListenerId = 1;
        Transport* transport = nullptr;
        OutputMeter* outputMeter = nullptr;
        TransportSettings transportState;
        kits::KitCatalog* kitCatalog = nullptr;
        Storage* storage = nullptr;
        Revision nextRevision = 1;
        Revision savedRevision = 0;

        Result runTransportCommand (const Command&);

        // Around a change to the project during playback (SPEC §6.4): before
        // it, whether it's playing, forgetting any earlier wrap; after it, a
        // sentence saying how the change moved the playhead or stopped
        // playback, or nothing.
        bool playingBeforeChange();
        std::string playbackAfterChange (bool wasPlaying);
        edits::Plan plan (const project::Project&, const Command&) const;
        edits::Plan planRevert (const project::Project&, const Revert&) const;

        // Applies a command's edits, and says what they did.
        Edits applyCommand (std::vector<Edit> forward);
        void applyForward (const Edits&);
        void applyInverse (const Edits&);

        // A new entry, whose state is `revision`, or a new revision if 0.
        void record (const Author& author, std::string description, Edits edits, Revision revision = 0);
        void closeTransactionPart();
        void notify (const std::vector<Change>&);
    };
}
