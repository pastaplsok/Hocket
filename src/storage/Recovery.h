#pragma once

// The recovery copy during a session (SPEC §4.5): what to do at the start
// with a copy an earlier session left, and keeping this session's copy up to
// date while it runs. The copy on disk is RecoveryCopy's.
//
// A copy exists only while the session holds changes that aren't on disk: a
// few seconds after changes stop, it's written; once nothing is unsaved (a
// save, or undo back to the saved state), it's removed. But a copy holding
// recovered changes that were never saved, once they're gone from the project
// (the recovered entry undone, say), is filed with the kept saves as a
// discarded recovery before it goes, as --discard-recovery would (Lee,
// 2026-11-21): no recovered work is lost by an undo. So one found at the
// start holds changes a session ended without saving, by a crash or by
// quitting, and plain --play won't play until the person says what to do
// with it (Lee, 2026-11-21): --recover plays it, --discard-recovery files it
// with the kept saves and plays the files as saved.
//
// Message thread only.

#include "core/commands/CommandBus.h"
#include "storage/ProjectFolder.h"
#include "storage/RecoveryCopy.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace hocket::storage
{
    // What the person asked --play to do with a recovery copy an earlier session left.
    enum class RecoveryChoice
    {
        none,    // nothing: --play stops if there's one with unsaved changes in it
        recover, // --recover: play it
        discard  // --discard-recovery: file it with the kept saves, and play the files as saved
    };

    struct RecoveryStart
    {
        bool proceed = true; // false: don't play; the message says why, and what to do
        std::string message; // what to tell the person, if anything

        // With recover: the copy's project, to bring in as one command
        // replacing the project (ReplaceProject), so the person can undo it,
        // and it's unsaved; and that command's description.
        std::optional<project::Project> recovered;
        std::string description;
    };

    // At the start of a session, once the project has loaded: looks for a
    // recovery copy of it, and acts on the person's choice. A copy that holds
    // nothing the project's files don't is removed, whatever the choice.
    RecoveryStart startRecovery (RecoveryCopy& copy, ProjectFolder& folder, const project::Project& loaded, RecoveryChoice choice);

    struct UnloadableStart
    {
        bool proceed = false; // false: don't play; the message says why, and what there is to go back to
        std::string message;

        // With proceed: the session's project, the recovery copy's, which
        // starts unsaved (CommandBus::markUnsaved), and the folder as it is,
        // to save into (ProjectFolder::asItIs).
        std::optional<project::Project> recovered;
        std::optional<ProjectFolder> folder;
    };

    // At the start of a session whose project's files don't load (a hand edit
    // broke project.json, say), once their problems have been printed. With
    // --recover and a copy that reads, the session starts as the copy, unsaved
    // (Lee, 2026-11-22): there's no loadable project for an undo to go back
    // to. Otherwise nothing plays, and the message says where the recovery
    // copy and the kept saves are, and how to reach them; --discard-recovery
    // files the copy with the kept saves first.
    UnloadableStart startUnloadable (RecoveryCopy& copy, const std::filesystem::path& folder, RecoveryChoice choice);

    // Keeps the session's recovery copy up to date: writes it once changes
    // have stopped for settleTime (or have gone on for longestWait), and
    // removes it once nothing is unsaved.
    class RecoveryKeeper
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr Clock::duration settleTime = std::chrono::seconds (3);
        static constexpr Clock::duration longestWait = std::chrono::seconds (30);
        static constexpr Clock::duration retryTime = std::chrono::seconds (10); // after a write or remove fails

        // `holdsSession`: the copy on disk already holds the session's
        // project as it is now: it was just recovered, as the newest history
        // entry. Otherwise changes already unsaved are copied once they've
        // settled from the first poll.
        RecoveryKeeper (commands::CommandBus& bus, ProjectFolder& folder, RecoveryCopy& copy, bool holdsSession);

        // Call often: --play does every 50 ms. Returns a sentence to tell the
        // person: a problem writing or removing the copy, said once until it
        // changes, and that the copy is written again once it is.
        std::optional<std::string> poll (Clock::time_point now = Clock::now());

        // At a clean exit. With nothing unsaved, removes the copy (filing it
        // first if it holds recovered changes that were never saved, and
        // saying so). Otherwise makes sure the copy holds the project as it
        // is now, and says where it is and how to get it back.
        std::string finish();

    private:
        commands::CommandBus& bus;
        ProjectFolder& folder;
        RecoveryCopy& copy;

        commands::CommandBus::Revision seen = 0; // the revision when it last looked
        Clock::time_point lastChange {};
        std::optional<Clock::time_point> waitingSince; // a change not yet in the copy, since when
        std::optional<Clock::time_point> retryAt;      // after a failure

        // What the copy on disk holds, if this session wrote it (or recovered it).
        std::optional<std::map<std::string, std::string>> copyHolds;

        // The history entry --recover made, until its changes are saved or
        // filed with the kept saves.
        std::optional<int> recoveredEntry;

        std::string lastProblem; // the last one reported

        std::optional<std::string> update (Clock::time_point now);
        std::optional<std::string> removeCopy (Clock::time_point now);

        // Before the copy goes: if it holds recovered changes that were never
        // saved, files it with the kept saves and returns what to say. Sets
        // `problem` if it couldn't, and then the copy must stay.
        std::string fileIfRecovered (std::string& problem);
        std::optional<std::string> report (std::string problem, Clock::time_point now);
        std::optional<std::string> recovered();
    };
}
