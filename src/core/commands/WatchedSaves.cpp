#include "core/commands/WatchedSaves.h"

namespace hocket::commands
{
namespace
{
    std::optional<std::string> canonicalText (const project::Project& project, std::string_view clipId)
    {
        if (const auto* clip = project::findClip (project, clipId))
            return notes::write (clip->content, project.meter);

        return {};
    }
}

WatchedSaves::WatchedSaves (CommandBus& commandBus, Author personAtTheDaw)
    : bus (commandBus), person (std::move (personAtTheDaw))
{
    for (const auto& track : bus.project().tracks)
        for (const auto& clip : track.clips)
            matchedFile[clip.id] = notes::write (clip.content, bus.project().meter);
}

WatchedSaves::Outcome WatchedSaves::save (const std::string& clipId, const std::string& text)
{
    const auto before = canonicalText (bus.project(), clipId);
    auto result = bus.execute (WriteClip { clipId, text }, person);

    if (! result.ok || ! before)
        return { std::move (result), {} };

    // Something other than the file changed the clip since they last matched.
    const auto matched = matchedFile.find (clipId);
    const auto changedElsewhere = matched == matchedFile.end() || *before != matched->second;
    const auto after = *canonicalText (bus.project(), clipId);
    matchedFile[clipId] = after;

    if (! changedElsewhere || after == *before)
        return { std::move (result), {} };

    return { std::move (result), warningFor (clipId) };
}

std::string WatchedSaves::warningFor (const std::string& clipId) const
{
    // The save is the newest change to the clip, unless it went into an open
    // transaction; what it replaced came before it.
    const auto changes = bus.changesTo (clipId);
    const auto saveEntry = ! changes.empty() && changes.front().author == person ? &changes.front() : nullptr;
    const HistoryEntry* replaced = nullptr;

    for (std::size_t i = saveEntry != nullptr ? 1 : 0; i < changes.size() && replaced == nullptr; ++i)
        if (changes[i].author != person)
            replaced = &changes[i];

    auto warning = "warning: this save replaced changes made to clip " + clipId + " since the file was last loaded or saved";

    if (replaced != nullptr)
        warning += " (the latest: #" + std::to_string (replaced->id) + " \"" + replaced->description + "\" by " + displayName (replaced->author) + ")";

    warning += ". Hocket doesn't write clip files yet, so the file didn't have them.";

    if (saveEntry != nullptr)
        warning += " Undoing #" + std::to_string (saveEntry->id) + " brings them back.";

    return warning;
}
}
