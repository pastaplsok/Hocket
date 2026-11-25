#include "storage/WatchedFolder.h"

#include "core/Paths.h"

#include <algorithm>

namespace hocket::storage
{
WatchedFolder::WatchedFolder (commands::CommandBus& commandBus, ProjectFolder& projectFolder, commands::Author personAtTheDaw,
                              ProjectWatcher::Clock::duration settle)
    : bus (commandBus), folder (projectFolder), person (personAtTheDaw), settleTime (settle), watcher (projectFolder, settle),
      saves (commandBus, std::move (personAtTheDaw))
{
}

std::vector<WatchedFolder::Report> WatchedFolder::poll (ProjectWatcher::Clock::time_point now)
{
    std::vector<Report> reports;

    for (const auto& change : watcher.poll (bus.project(), now))
    {
        // A clip file project.json's change already read from disk is in.
        if (change.file != "project.json" && folder.knownStamp (change.file) == change.stamp)
            continue;

        reports.push_back (change.file == "project.json" ? takeProject (change) : takeClip (change));
    }

    // A project.json refused, still as it was: read again once a clip file
    // it names has changed and settled.
    if (refused)
    {
        const auto projectPath = folder.path() / pathFromUtf8 ("project.json");
        const auto changed = std::ranges::any_of (refused->files, [this] (const auto& file)
        {
            return project::FileStamp::of (folder.path() / pathFromUtf8 (file.first)) != file.second;
        });

        if (project::FileStamp::of (projectPath) != refused->stamp)
        {
            refused.reset(); // saved again: the watcher reports that
        }
        else if (! changed)
        {
            refused->changedAt.reset();
        }
        else if (! refused->changedAt)
        {
            refused->changedAt = now;
        }
        else if (now - *refused->changedAt >= settleTime)
        {
            const auto stamp = refused->stamp;
            refused.reset();
            reports.push_back (takeProject ({ "project.json", readTextFile (projectPath), stamp }));
        }
    }

    return reports;
}

WatchedFolder::Report WatchedFolder::takeProject (const ProjectWatcher::Change& change)
{
    if (! change.text)
        return { change.file, false, "project.json is gone or can't be read, so the session carries on as it is. Saving won't write over "
                                     "a project.json changed or deleted outside Hocket unless asked to (overwrite)" };

    // Each clip file as the session has it, unsaved changes and all, unless
    // it changed on disk too, or is new to the project: then as it is there.
    struct ReadFromDisk
    {
        std::string file;
        project::FileStamp stamp;
    };

    std::vector<ReadFromDisk> fromDisk;
    std::map<std::string, project::FileStamp> named; // every clip file it names, as it is now
    const auto& session = bus.project();

    const auto loaded = project::parse (*change.text, [&] (const std::string& file) -> std::optional<std::string>
    {
        const auto path = folder.path() / pathFromUtf8 (file);
        const auto stamp = project::FileStamp::of (path);
        named[file] = stamp;

        if (folder.knownStamp (file) == stamp)
            if (auto text = project::clipFileText (session, file))
                return text;

        auto text = readTextFile (path);

        if (text)
            fromDisk.push_back ({ file, stamp });

        return text;
    });

    // Until it, or a clip file it names, is saved again.
    const auto readAgain = "\nIt's read again once it, or a clip file it names, is saved.";

    if (! loaded.project)
    {
        std::string message = "project.json was saved outside Hocket, but it doesn't read, so the session carries on as it was:";

        for (const auto& error : loaded.errors)
            message += "\n  " + project::formatDiagnostic (error);

        refused = Refused { change.stamp, std::move (named) };
        return { change.file, false, message + readAgain };
    }

    // The session's project.json, and the file as Hocket last read or wrote
    // it: they differ when the session holds changes to the project that
    // weren't saved, which a save made to the file on disk replaces.
    const auto sessionJson = project::write (session).at ("project.json");
    const auto knownJson = folder.knownText ("project.json");
    const auto unsaved = knownJson && *knownJson != sessionJson;

    auto result = bus.execute (commands::ReplaceProject { *loaded.project, "project.json saved outside Hocket" }, person);
    std::optional<std::string> warning;

    if (result.ok)
    {
        const auto files = project::write (bus.project());
        folder.noteRead ("project.json", change.stamp, files.at ("project.json"));

        for (const auto& read : fromDisk)
            if (const auto text = files.find (read.file); text != files.end())
                folder.noteRead (read.file, read.stamp, text->second);

        if (unsaved && files.at ("project.json") != sessionJson)
            warning = replacedWarning();

        refused.reset();
    }
    else
    {
        refused = Refused { change.stamp, std::move (named) };
        result.message += readAgain;
    }

    return { change.file, result.ok, std::move (result.message), std::move (warning) };
}

std::string WatchedFolder::replacedWarning() const
{
    // The save is the newest change to the project's structure; what it
    // replaced came before it, by someone else.
    const auto changes = bus.structureChanges();
    const auto* saveEntry = ! changes.empty() && changes.front().author == person ? &changes.front() : nullptr;
    const commands::HistoryEntry* replaced = nullptr;

    for (std::size_t i = saveEntry != nullptr ? 1 : 0; i < changes.size() && replaced == nullptr; ++i)
        if (changes[i].author != person)
            replaced = &changes[i];

    std::string warning = "warning: this save of project.json replaced changes made to the project since it was last loaded or saved";

    if (replaced != nullptr)
        warning += " (the latest: #" + std::to_string (replaced->id) + " \"" + replaced->description + "\" by " + commands::displayName (replaced->author) + ")";

    warning += ". They weren't saved, so the file didn't have them.";

    if (saveEntry != nullptr)
        warning += " Undoing #" + std::to_string (saveEntry->id) + " brings them back.";

    return warning;
}

WatchedFolder::Report WatchedFolder::takeClip (const ProjectWatcher::Change& change)
{
    if (! change.text)
        return { change.file, false, "the file is gone or can't be read; the clips using it keep their notes" };

    // What the file held as Hocket last saved or read it, for the warning.
    if (auto known = folder.knownText (change.file))
        saves.matched (change.file, std::move (*known));

    auto outcome = saves.save (change.file, *change.text);

    if (outcome.result.ok)
        if (const auto text = project::clipFileText (bus.project(), change.file))
            folder.noteRead (change.file, change.stamp, *text);

    return { change.file, outcome.result.ok, std::move (outcome.result.message), std::move (outcome.warning) };
}
}
