#include "storage/WatchedFolder.h"

#include "core/Paths.h"

namespace hocket::storage
{
WatchedFolder::WatchedFolder (commands::CommandBus& commandBus, ProjectFolder& projectFolder, commands::Author personAtTheDaw,
                              ProjectWatcher::Clock::duration settleTime)
    : bus (commandBus), folder (projectFolder), person (personAtTheDaw), watcher (projectFolder, settleTime), saves (commandBus, std::move (personAtTheDaw))
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
    const auto& session = bus.project();

    const auto loaded = project::parse (*change.text, [&] (const std::string& file) -> std::optional<std::string>
    {
        const auto path = folder.path() / pathFromUtf8 (file);
        const auto stamp = project::FileStamp::of (path);

        if (folder.knownStamp (file) == stamp)
            if (auto text = project::clipFileText (session, file))
                return text;

        auto text = readTextFile (path);

        if (text)
            fromDisk.push_back ({ file, stamp });

        return text;
    });

    if (! loaded.project)
    {
        std::string message = "project.json was saved outside Hocket, but it doesn't read, so the session carries on as it was:";

        for (const auto& error : loaded.errors)
            message += "\n  " + project::formatDiagnostic (error);

        return { change.file, false, message };
    }

    auto result = bus.execute (commands::ReplaceProject { *loaded.project, "project.json saved outside Hocket" }, person);

    if (result.ok)
    {
        const auto files = project::write (bus.project());
        folder.noteRead ("project.json", change.stamp, files.at ("project.json"));

        for (const auto& read : fromDisk)
            if (const auto text = files.find (read.file); text != files.end())
                folder.noteRead (read.file, read.stamp, text->second);
    }

    return { change.file, result.ok, std::move (result.message) };
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
