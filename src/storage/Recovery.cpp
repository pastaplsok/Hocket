#include "storage/Recovery.h"

#include "core/Paths.h"

#include <algorithm>
#include <limits>

namespace hocket::storage
{
namespace
{
    std::string quoted (const std::filesystem::path& path)
    {
        return "'" + utf8FromPath (path) + "'";
    }

    std::string withoutStop (std::string text)
    {
        while (! text.empty() && text.back() == '.')
            text.pop_back();

        return text;
    }

    // When the project's files last changed on disk: the latest of their dates.
    std::optional<commands::SaveTime> filesChanged (const std::filesystem::path& folder, const project::Project& loaded)
    {
        std::optional<commands::SaveTime> latest;

        for (const auto& entry : project::write (loaded))
        {
            std::error_code error;
            const auto modified = std::filesystem::last_write_time (folder / pathFromUtf8 (entry.first), error);

            if (error)
                continue;

            const auto time = std::chrono::floor<std::chrono::seconds> (std::chrono::clock_cast<std::chrono::system_clock> (modified));

            if (! latest || time > *latest)
                latest = time;
        }

        return latest;
    }

    // "project.json:3:5: ... (and 2 more problems)"
    std::string firstProblem (const project::LoadResult& read)
    {
        auto problem = project::formatDiagnostic (read.errors.front());

        if (const auto more = read.errors.size() - 1; more > 0)
            problem += " (and " + std::to_string (more) + (more == 1 ? " more problem)" : " more problems)");

        return problem;
    }

    // " for 'D:\Music\song'": a copy written for another folder than this
    // one, found by the project's ID (RecoveryCopy::adoptMoved). Every
    // message about the copy names that folder.
    std::string forFolder (const RecoveryCopy& copy, const RecoveryCopy::Found& found)
    {
        return found.writtenFor == copy.project() ? std::string() : " for '" + found.writtenFor + "'";
    }

    // What that means, said where --play stops and asks: the folder may have
    // been copied, not moved, and the changes the other's.
    std::string writtenElsewhere (const RecoveryCopy& copy, const RecoveryCopy::Found& found)
    {
        if (found.writtenFor == copy.project())
            return {};

        return "It was written for the folder '" + found.writtenFor + "', which has this project's ID (" + std::string (projectIdFile)
             + "): this folder was moved or renamed from there, or copied from it. If it was copied, these are that folder's changes.";
    }

    // What plain --play says, and plays nothing, when an earlier session left
    // unsaved changes in a recovery copy.
    std::string askWhatToDo (const RecoveryCopy& copy, const RecoveryCopy::Found& found, const project::LoadResult& read, const ProjectFolder& folder,
                             const project::Project& loaded)
    {
        const auto written = commands::localTimeText (found.written) + forFolder (copy, found);
        std::string text = "This project has a recovery copy, written " + written
                         + ", holding changes that were never saved: the session that made them ended without saving them, by a crash or by quitting.\n";

        if (const auto elsewhere = writtenElsewhere (copy, found); ! elsewhere.empty())
            text += elsewhere + "\n";

        if (const auto changed = filesChanged (folder.path(), loaded); changed && *changed > found.written)
            text += "It's older than the project's files, which changed at " + commands::localTimeText (*changed)
                  + ", after it was written: they were saved or edited since. Recovering it would put its version in place of theirs.\n";
        else
            text += "It's newer than the project's files.\n";

        if (read.project)
            text += commands::replacementChanges (loaded, *read.project) + ".\n";
        else
            text += "It doesn't read as a project, so it can't be played: " + firstProblem (read) + "\n";

        text += "Nothing was played. To go on, run --play again with one of these:\n";

        if (read.project)
            text += "  --recover            play the recovery copy: its changes come in as one history entry, unsaved, which undo takes back\n";

        text += "  --discard-recovery   play the files as saved, filing the recovery copy with the project's kept saves, so revert can still bring it back\n";
        text += "The copy is in " + quoted (found.folder) + ".";
        return text;
    }
}

namespace
{
    RecoveryStart decide (RecoveryCopy& copy, ProjectFolder& folder, const project::Project& loaded, RecoveryChoice choice);
    UnloadableStart startUnloadableFound (RecoveryCopy& copy, const std::filesystem::path& folder, RecoveryChoice choice);
}

RecoveryStart startRecovery (RecoveryCopy& copy, ProjectFolder& folder, const project::Project& loaded, RecoveryChoice choice)
{
    // The copy kept for the folder's old path, if it was moved, renamed or
    // copied since: taken over, and named as written for that folder.
    copy.adoptMoved();
    return decide (copy, folder, loaded, choice);
}

namespace
{
RecoveryStart decide (RecoveryCopy& copy, ProjectFolder& folder, const project::Project& loaded, RecoveryChoice choice)
{
    const auto found = copy.find();

    if (! found)
    {
        if (choice == RecoveryChoice::recover)
            return { false, "--recover: there's no recovery copy of this project, so there's nothing to recover: no session left changes unsaved. "
                            "--play without --recover plays its files." };

        if (choice == RecoveryChoice::discard)
            return { true, "--discard-recovery: there's no recovery copy of this project to discard." };

        return {};
    }

    const auto written = commands::localTimeText (found->written) + forFolder (copy, *found);
    const auto read = project::load (found->folder);

    // One a crash left between a save and the copy's removal, say.
    if (read.project && project::write (*read.project) == project::write (loaded))
    {
        RecoveryStart start { true, "Found a recovery copy written " + written + " that holds nothing the project's files don't, so it's removed." };

        if (const auto problem = copy.remove(); ! problem.empty())
            start.message = "Found a recovery copy written " + written + " that holds nothing the project's files don't, but couldn't remove it: "
                          + withoutStop (problem) + ".";

        return start;
    }

    switch (choice)
    {
        case RecoveryChoice::none:
            return { false, askWhatToDo (copy, *found, read, folder, loaded) };

        case RecoveryChoice::recover:
            if (! read.project)
                return { false, "--recover: the recovery copy written " + written + " doesn't read as a project, so it can't be played: " + firstProblem (read)
                                    + "\nIt's in " + quoted (found->folder) + ". --discard-recovery files it with the project's kept saves, "
                                    + "where its files can be looked at and mended, and plays the project's files as saved." };

            return { true, {}, read.project, "recovered the unsaved changes in the recovery copy written " + written };

        case RecoveryChoice::discard:
            break;
    }

    std::string problem;
    const auto kept = folder.keepRecoveryCopy (found->folder, found->written, problem);

    if (! kept)
        return { false, "--discard-recovery: couldn't file the recovery copy written " + written + " with the project's kept saves, so it's left as it is, in "
                            + quoted (found->folder) + ": " + withoutStop (problem) + ". Nothing was played." };

    RecoveryStart start { true, "Filed the recovery copy written " + written + " as " + commands::describe (*kept)
                                    + ", so revert can still bring it back. Playing the project's files as saved." };

    if (const auto removed = copy.remove(); ! removed.empty())
        start.message += "\nCouldn't remove it from the recovery folder, though: " + withoutStop (removed)
                       + ". The next --play will ask about it again; it's safe to delete it by hand.";

    return start;
}
}

UnloadableStart startUnloadable (RecoveryCopy& copy, const std::filesystem::path& folder, RecoveryChoice choice)
{
    copy.adoptMoved(); // as startRecovery does
    return startUnloadableFound (copy, folder, choice);
}

namespace
{
UnloadableStart startUnloadableFound (RecoveryCopy& copy, const std::filesystem::path& folder, RecoveryChoice choice)
{
    auto projectFolder = ProjectFolder::asItIs (folder);
    const auto found = copy.find();
    const auto read = found ? project::load (found->folder) : project::LoadResult {};
    const auto written = found ? commands::localTimeText (found->written) + forFolder (copy, *found) : std::string();

    if (choice == RecoveryChoice::recover && found && read.project)
    {
        UnloadableStart start { true, "The project's files don't load, so the session starts as the recovery copy written " + written
                                          + ", unsaved: the first save keeps the files on disk as they are, as a kept save, then writes this version." };
        start.recovered = read.project;
        start.folder.emplace (std::move (projectFolder));
        return start;
    }

    std::string text;
    auto haveCopy = found.has_value();

    if (choice == RecoveryChoice::discard && found)
    {
        std::string problem;

        if (const auto kept = projectFolder.keepRecoveryCopy (found->folder, found->written, problem))
        {
            text = "Filed the recovery copy written " + written + " as " + commands::describe (*kept) + ".\n";
            haveCopy = false;

            if (const auto removed = copy.remove(); ! removed.empty())
                text += "Couldn't remove it from the recovery folder, though: " + withoutStop (removed) + ". It's safe to delete it by hand.\n";
        }
        else
        {
            text = "--discard-recovery: couldn't file the recovery copy written " + written + " with the kept saves, so it's left as it is: "
                 + withoutStop (problem) + ".\n";
        }
    }

    text += "The project's files don't load (their problems are above), so nothing was played.";

    if (choice == RecoveryChoice::recover && ! found)
        text += "\n--recover: there's no recovery copy of this project.";

    if (choice == RecoveryChoice::discard && ! found)
        text += "\n--discard-recovery: there's no recovery copy of this project to discard.";

    if (haveCopy)
    {
        text += "\nA recovery copy written " + written + " holds changes a session never saved";

        if (read.project)
            text += ". --play --recover plays it in place of the project's files, unsaved; the first save keeps those files as they are before writing "
                    "its version. --play --discard-recovery files it with the kept saves instead.";
        else
            text += ", but it doesn't read as a project either: " + firstProblem (read)
                  + ". --play --discard-recovery files it with the kept saves, where its files can be looked at and mended.";

        text += " It's in " + quoted (found->folder) + ".";

        if (const auto elsewhere = writtenElsewhere (copy, *found); ! elsewhere.empty())
            text += "\n" + elsewhere;
    }

    // Newest first, as the person would want them.
    auto saves = projectFolder.keptSaves();
    std::ranges::reverse (saves);
    constexpr std::size_t listedSaves = 3;

    if (! saves.empty())
    {
        text += "\nEarlier versions are kept whole in .hocket/saves; to go back to one by hand, copy its project.json and clips folder into the project's folder:";

        for (std::size_t i = 0; i < saves.size() && i < listedSaves; ++i)
            text += "\n  " + commands::describe (saves[i]) + ": " + quoted ((folder / pathFromUtf8 (saves[i].folder)).make_preferred());

        if (saves.size() > listedSaves)
            text += "\n  and " + std::to_string (saves.size() - listedSaves) + " earlier, in .hocket/saves.";
    }

    if (! found && saves.empty())
        text += "\nThere's no recovery copy and no kept save of it: mend its files by hand. --check lists their problems.";

    return { false, text };
}
}

//==============================================================================
RecoveryKeeper::RecoveryKeeper (commands::CommandBus& commandBus, ProjectFolder& projectFolder, RecoveryCopy& recoveryCopy, bool holdsSession)
    : bus (commandBus), folder (projectFolder), copy (recoveryCopy)
{
    // Changes already unsaved count from the first poll, as if just made.
    seen = holdsSession ? bus.revision() : std::numeric_limits<commands::CommandBus::Revision>::max();

    if (holdsSession)
    {
        copyHolds = project::write (bus.project());

        if (const auto history = bus.history(); ! history.empty())
            recoveredEntry = history.front().id;
    }
}

std::optional<std::string> RecoveryKeeper::poll (Clock::time_point now)
{
    if (const auto revision = bus.revision(); revision != seen)
    {
        seen = revision;
        lastChange = now;

        if (! waitingSince)
            waitingSince = now;
    }

    if (retryAt && now < *retryAt)
        return {};

    // Saved, or undone back to the save: nothing to keep. At once, so a
    // crash now doesn't leave a copy to ask about.
    if (! bus.hasUnsavedChanges())
    {
        waitingSince.reset();
        return removeCopy (now);
    }

    const auto due = retryAt || (waitingSince && (now - lastChange >= settleTime || now - *waitingSince >= longestWait));

    if (! due)
        return {};

    waitingSince.reset();
    return update (now);
}

std::optional<std::string> RecoveryKeeper::update (Clock::time_point now)
{
    auto files = project::write (bus.project());

    // Changes that are on disk after all (put back by hand, or taken in from
    // files saved outside Hocket): nothing to keep.
    if (folder.matchesKnown (files))
        return removeCopy (now);

    if (copyHolds && *copyHolds == files)
    {
        retryAt.reset();
        return {};
    }

    if (auto problem = copy.write (files); ! problem.empty())
        return report ("Couldn't write the recovery copy, so a crash now would lose the changes made since " + std::string (copyHolds ? "it was last written" : "the last save")
                           + ": " + withoutStop (problem) + ". Trying again every " + std::to_string (std::chrono::duration_cast<std::chrono::seconds> (retryTime).count())
                           + " s.",
                       now);

    copyHolds = std::move (files);
    return recovered();
}

std::string RecoveryKeeper::fileIfRecovered (std::string& problem)
{
    if (! recoveredEntry)
        return {};

    // Saved with the recovered entry in it: they're on disk, so there's nothing to file.
    const auto history = bus.history();
    const auto stillThere = std::ranges::any_of (history, [this] (const commands::HistoryEntry& entry) { return entry.id == *recoveredEntry; });

    if (stillThere && ! bus.hasUnsavedChanges())
    {
        recoveredEntry.reset();
        return {};
    }

    const auto found = copy.find();

    if (! found)
    {
        recoveredEntry.reset();
        return {};
    }

    const auto kept = folder.keepRecoveryCopy (found->folder, found->written, problem);

    if (! kept)
        return {};

    // Once is enough: the changes are in the kept saves now.
    recoveredEntry.reset();
    return "The recovered changes aren't in the project any more, and were never saved, so the recovery copy written " + commands::localTimeText (found->written)
         + " is filed as " + commands::describe (*kept) + ": revert brings them back.";
}

std::optional<std::string> RecoveryKeeper::removeCopy (Clock::time_point now)
{
    if (! copyHolds)
        return recovered();

    std::string problem;
    const auto filed = fileIfRecovered (problem);

    if (! problem.empty())
        return report ("Couldn't file the recovery copy with the kept saves, so it stays as it is, holding the recovered changes: " + withoutStop (problem)
                           + ". Trying again every " + std::to_string (std::chrono::duration_cast<std::chrono::seconds> (retryTime).count()) + " s.",
                       now);

    if (const auto removed = copy.remove(); ! removed.empty())
    {
        const auto said = report ("Couldn't remove the recovery copy, though nothing is unsaved: " + withoutStop (removed) + ". Trying again every "
                                      + std::to_string (std::chrono::duration_cast<std::chrono::seconds> (retryTime).count()) + " s.",
                                  now);

        if (filed.empty())
            return said;

        return said ? filed + "\n" + *said : filed;
    }

    copyHolds.reset();
    const auto back = recovered();

    if (filed.empty())
        return back;

    return back ? filed + "\n" + *back : filed;
}

std::optional<std::string> RecoveryKeeper::report (std::string problem, Clock::time_point now)
{
    retryAt = now + retryTime;

    if (problem == lastProblem)
        return {};

    lastProblem = problem;
    return problem;
}

std::optional<std::string> RecoveryKeeper::recovered()
{
    retryAt.reset();

    if (lastProblem.empty())
        return {};

    lastProblem.clear();
    return std::string (copyHolds ? "The recovery copy is written again." : "The recovery copy is removed.");
}

std::string RecoveryKeeper::finish()
{
    auto files = project::write (bus.project());

    if (! bus.hasUnsavedChanges() || folder.matchesKnown (files))
    {
        if (! copyHolds)
            return {};

        std::string problem;
        const auto filed = fileIfRecovered (problem);

        if (! problem.empty())
            return "Couldn't file the recovery copy with the kept saves, so it's left as it is, holding the recovered changes, and the next --play "
                   "will ask about it: " + withoutStop (problem) + ".";

        if (const auto removed = copy.remove(); ! removed.empty())
            return (filed.empty() ? std::string() : filed + "\n") + "Couldn't remove the recovery copy, though nothing is unsaved: " + withoutStop (removed)
                 + ". The next --play will ask about it; it's safe to delete it by hand.";

        copyHolds.reset();
        return filed;
    }

    if (! copyHolds || *copyHolds != files)
    {
        if (const auto problem = copy.write (files); ! problem.empty())
            return "Couldn't keep the unsaved changes in a recovery copy, so they're lost: " + withoutStop (problem) + "."
                 + (copyHolds ? " The recovery copy written before holds them as they were then." : "");

        copyHolds = std::move (files);
    }

    const auto found = copy.find();
    return "Unsaved changes are kept in a recovery copy" + (found ? ", " + quoted (found->folder) : std::string())
         + ". --play --recover plays them next time; --play --discard-recovery files them with the project's kept saves.";
}
}
