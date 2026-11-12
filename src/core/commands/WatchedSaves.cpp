#include "core/commands/WatchedSaves.h"

namespace hocket::commands
{
WatchedSaves::WatchedSaves (CommandBus& commandBus, Author personAtTheDaw)
    : bus (commandBus), person (std::move (personAtTheDaw))
{
    for (const auto& entry : bus.project().notesFiles)
        matchedFile[entry.first] = *project::clipFileText (bus.project(), entry.first);
}

WatchedSaves::Outcome WatchedSaves::save (const std::string& file, const std::string& text)
{
    // The write goes to the file's first clip; it changes every clip linked to it.
    const auto clips = project::clipsUsing (bus.project(), file);

    if (clips.empty())
        return { { false, "no clip uses " + file + " any more, so the save wasn't played" }, {} };

    const auto before = project::clipFileText (bus.project(), file);
    auto result = bus.execute (WriteClip { clips.front()->id, text }, person);

    if (! result.ok || ! before)
        return { std::move (result), {} };

    // Something other than the file changed its notes since they last matched.
    const auto matched = matchedFile.find (file);
    const auto changedElsewhere = matched == matchedFile.end() || *before != matched->second;
    const auto after = *project::clipFileText (bus.project(), file);
    matchedFile[file] = after;

    if (! changedElsewhere || after == *before)
        return { std::move (result), {} };

    return { std::move (result), warningFor (file) };
}

std::string WatchedSaves::warningFor (const std::string& file) const
{
    // The save is the newest change to the file, unless it went into an open
    // transaction; what it replaced came before it.
    const auto changes = bus.changesTo (file);
    const auto saveEntry = ! changes.empty() && changes.front().author == person ? &changes.front() : nullptr;
    const HistoryEntry* replaced = nullptr;

    for (std::size_t i = saveEntry != nullptr ? 1 : 0; i < changes.size() && replaced == nullptr; ++i)
        if (changes[i].author != person)
            replaced = &changes[i];

    auto warning = "warning: this save replaced changes made to " + file + " since it was last loaded or saved";

    if (replaced != nullptr)
        warning += " (the latest: #" + std::to_string (replaced->id) + " \"" + replaced->description + "\" by " + displayName (replaced->author) + ")";

    warning += ". Hocket doesn't write clip files yet, so the file didn't have them.";

    if (saveEntry != nullptr)
        warning += " Undoing #" + std::to_string (saveEntry->id) + " brings them back.";

    return warning;
}
}
