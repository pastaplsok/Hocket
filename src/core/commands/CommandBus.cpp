#include "core/commands/CommandBus.h"
#include "core/notes/NotesSyntax.h"

#include <cassert>

namespace hocket::commands
{
namespace
{
    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string doubleQuoted (std::string_view text)
    {
        return "\"" + std::string (text) + "\"";
    }

    std::string barsText (int bars)
    {
        return std::to_string (bars) + (bars == 1 ? "bar" : "bars");
    }

    std::string countText (std::size_t count, std::string_view noun)
    {
        return std::to_string (count) + " " + std::string (noun) + (count == 1 ? "" : "s");
    }

    const project::Clip* findClip (const project::Project& project, std::string_view id, const project::Track** trackFound = nullptr)
    {
        for (const auto& track : project.tracks)
        {
            for (const auto& clip : track.clips)
            {
                if (clip.id == id)
                {
                    if (trackFound != nullptr)
                        *trackFound = &track;

                    return &clip;
                }
            }
        }

        return nullptr;
    }

    project::Clip& clipById (project::Project& project, std::string_view id)
    {
        auto* clip = const_cast<project::Clip*> (findClip (project, id));
        assert (clip != nullptr); // edits are only made for clips that validation found
        return *clip;
    }

    // "there's no clip 'clp-x'; the clips are ...", or a hint when it's another kind of ID.
    std::string unknownClipMessage (const project::Project& project, std::string_view id)
    {
        std::string clipIds;

        for (const auto& track : project.tracks)
        {
            if (track.id == id)
                return singleQuoted (id) + " is a track ID; write_clip needs a clip ID like 'clp-bass'";

            for (const auto& device : track.devices)
                if (device.id == id)
                    return singleQuoted (id) + " is a device ID; write_clip needs a clip ID like 'clp-bass'";

            for (const auto& clip : track.clips)
                clipIds += (clipIds.empty() ? "" : ", ") + singleQuoted (clip.id);
        }

        return "there's no clip " + singleQuoted (id) + "; "
             + (clipIds.empty() ? std::string ("the project has no clips yet") : "the clips are " + clipIds);
    }
}

//==============================================================================
CommandBus::CommandBus (project::Project project) : currentProject (std::move (project))
{
}

Result CommandBus::execute (const Command& command, const std::string& author)
{
    if (author.empty())
        return { false, "commands need an author, e.g. \"Lee\" or \"Claude\"" };

    if (std::holds_alternative<Play> (command) || std::holds_alternative<Stop> (command))
        return runTransportCommand (command);

    std::vector<Edit> edits;
    std::string description;
    auto result = validate (std::get<WriteClip> (command), edits, description);

    // Invalid, or valid but changes nothing.
    if (! result.ok || edits.empty())
        return result;

    // Another author's command never waits for an open transaction (SPEC §3.1).
    if (transaction && transaction->author != author)
        closeTransactionPart();

    std::vector<Edit> inverse;

    for (const auto& edit : edits)
        inverse.push_back (apply (edit));

    // Undone entries assumed the project as it was; after a new edit they can't be redone.
    undone.clear();

    if (transaction && transaction->author == author)
    {
        transaction->forward.insert (transaction->forward.end(), edits.begin(), edits.end());
        transaction->inverse.insert (transaction->inverse.end(), inverse.begin(), inverse.end());
        return { true, description + " (in transaction " + doubleQuoted (transaction->name) + ")" };
    }

    const auto id = nextEntryId;
    record (author, description, std::move (edits), std::move (inverse));
    return { true, "#" + std::to_string (id) + " " + description };
}

Result CommandBus::runTransportCommand (const Command& command)
{
    const auto name = std::holds_alternative<Play> (command) ? "play" : "stop";

    if (transport == nullptr)
        return { false, std::string (name) + ": there's no audio engine running, so playback can't be started or stopped here" };

    return std::holds_alternative<Play> (command) ? transport->play() : transport->stop();
}

Result CommandBus::validate (const WriteClip& command, std::vector<Edit>& edits, std::string& description) const
{
    const project::Track* track = nullptr;
    const auto* clip = findClip (currentProject, command.clipId, &track);

    if (clip == nullptr)
        return { false, "write_clip: " + unknownClipMessage (currentProject, command.clipId) };

    auto parsed = notes::parse (command.notes, currentProject.meter);

    if (! parsed.clip)
    {
        auto message = "write_clip: the new text for clip " + singleQuoted (clip->id) + " has "
                     + countText (parsed.errors.size(), "error") + ", so nothing was changed:";

        for (const auto& error : parsed.errors)
            message += "\n" + notes::formatDiagnostic (error, clip->file);

        return { false, message };
    }

    // A longer @length mustn't run into the next clip on the track.
    const auto meter = currentProject.meter;
    const auto newEnd = clip->start + parsed.clip->lengthInBars * meter.ticksPerBar();
    const project::Clip* next = nullptr;

    for (const auto& other : track->clips)
        if (&other != clip && other.start >= clip->start && (next == nullptr || other.start < next->start))
            next = &other;

    if (next != nullptr && next->start < newEnd)
    {
        const auto maxBars = static_cast<int> ((next->start - clip->start) / meter.ticksPerBar());

        return { false, "write_clip: with @length " + barsText (parsed.clip->lengthInBars) + ", clip " + singleQuoted (clip->id)
                            + " would run from " + notes::formatPosition (clip->start, meter) + " to " + notes::formatPosition (newEnd, meter)
                            + ", into clip " + singleQuoted (next->id) + " at " + notes::formatPosition (next->start, meter)
                            + ". Clips on a track can't overlap, so make @length at most " + barsText (maxBars) + ". Nothing was changed" };
    }

    if (*parsed.clip == clip->content)
        return { true, "clip " + singleQuoted (clip->id) + " already has exactly this text; nothing changed" };

    description = "wrote clip " + clip->id + " (" + countText (parsed.clip->notes.size(), "note") + ")";
    edits.push_back (SetClipContent { clip->id, std::move (*parsed.clip) });
    return { true, {} };
}

CommandBus::Edit CommandBus::apply (const Edit& edit)
{
    return std::visit ([this] (const SetClipContent& set) -> Edit
    {
        auto& clip = clipById (currentProject, set.clipId);
        SetClipContent inverse { set.clipId, std::move (clip.content) };
        clip.content = set.content;
        notify (ClipContentChanged { set.clipId });
        return inverse;
    }, edit);
}

void CommandBus::record (const std::string& author, std::string description, std::vector<Edit> forward, std::vector<Edit> inverse)
{
    done.push_back ({ { nextEntryId++, author, std::move (description) }, std::move (forward), std::move (inverse) });
    undone.clear();
}

void CommandBus::closeTransactionPart()
{
    if (! transaction || transaction->forward.empty())
        return;

    record (transaction->author, transaction->name + " (part " + std::to_string (transaction->part) + ")",
            std::move (transaction->forward), std::move (transaction->inverse));

    transaction->forward.clear();
    transaction->inverse.clear();
    ++transaction->part;
}

//==============================================================================
Result CommandBus::beginTransaction (const std::string& author, const std::string& name)
{
    if (author.empty())
        return { false, "transactions need an author, e.g. \"Lee\" or \"Claude\"" };

    if (name.empty())
        return { false, "a transaction needs a name that says what it does, e.g. \"wrote bridge\"; it becomes the undo step's name" };

    if (transaction)
    {
        if (transaction->author == author)
            return { false, "you already have an open transaction " + doubleQuoted (transaction->name) + "; commit or roll it back first" };

        return { false, transaction->author + " has an open transaction " + doubleQuoted (transaction->name)
                            + ", and only one can be open at a time. Try again once it's committed or rolled back; "
                              "single commands don't need to wait" };
    }

    transaction = Transaction { author, name };
    return { true, "began transaction " + doubleQuoted (name) };
}

Result CommandBus::commitTransaction (const std::string& author)
{
    if (! transaction || transaction->author != author)
        return { false, "you don't have an open transaction to commit" };

    auto finished = std::move (*transaction);
    transaction.reset();

    if (finished.forward.empty())
        return { true, finished.part == 1 ? "committed " + doubleQuoted (finished.name) + " with no changes; nothing was added to the history"
                                          : "committed " + doubleQuoted (finished.name) + "; its earlier parts are already in the history" };

    const auto name = finished.part == 1 ? finished.name : finished.name + " (part " + std::to_string (finished.part) + ")";
    const auto id = nextEntryId;
    record (finished.author, name, std::move (finished.forward), std::move (finished.inverse));
    return { true, "#" + std::to_string (id) + " " + name };
}

Result CommandBus::rollbackTransaction (const std::string& author)
{
    if (! transaction || transaction->author != author)
        return { false, "you don't have an open transaction to roll back" };

    auto finished = std::move (*transaction);
    transaction.reset();

    for (auto i = finished.inverse.size(); i-- > 0;)
        apply (finished.inverse[i]);

    auto message = "rolled back " + doubleQuoted (finished.name);

    if (finished.part > 1)
        message += "; its earlier parts were already in the history because other changes came in between, so undo those separately if needed";

    return { true, message };
}

//==============================================================================
Result CommandBus::undo (const std::string& author, UndoScope scope)
{
    if (transaction)
    {
        if (transaction->author == author)
            return { false, "can't undo while your transaction " + doubleQuoted (transaction->name) + " is open; roll it back instead, or commit it first" };

        if (scope == UndoScope::callersOwn && ! transaction->forward.empty())
            return { false, "didn't undo: the most recent change is part of " + transaction->author + "'s open transaction "
                                + doubleQuoted (transaction->name) + ", not yours. Leave it, or undo with force to undo it anyway" };

        closeTransactionPart();
    }

    if (done.empty())
        return { false, "nothing to undo: the history is empty" };

    const auto& top = done.back().info;

    if (scope == UndoScope::callersOwn && top.author != author)
        return { false, "didn't undo: the most recent change, #" + std::to_string (top.id) + " " + doubleQuoted (top.description)
                            + ", was made by " + top.author + ", not " + author + ". Undo only takes back the most recent change, so this would undo "
                            + top.author + "'s work. Leave it, or undo with force to undo it anyway" };

    auto entry = std::move (done.back());
    done.pop_back();

    for (auto i = entry.inverse.size(); i-- > 0;)
        apply (entry.inverse[i]);

    const auto message = "undid #" + std::to_string (entry.info.id) + " " + doubleQuoted (entry.info.description) + " by " + entry.info.author;
    undone.push_back (std::move (entry));
    return { true, message };
}

Result CommandBus::redo (const std::string& author)
{
    if (transaction && transaction->author == author)
        return { false, "can't redo while your transaction " + doubleQuoted (transaction->name) + " is open; commit or roll it back first" };

    if (undone.empty())
        return { false, "nothing to redo" };

    auto entry = std::move (undone.back());
    undone.pop_back();

    for (const auto& edit : entry.forward)
        apply (edit);

    const auto message = "redid #" + std::to_string (entry.info.id) + " " + doubleQuoted (entry.info.description) + " by " + entry.info.author;
    done.push_back (std::move (entry));
    return { true, message };
}

std::vector<HistoryEntry> CommandBus::history() const
{
    std::vector<HistoryEntry> entries;

    for (auto i = done.size(); i-- > 0;)
        entries.push_back (done[i].info);

    return entries;
}

int CommandBus::addListener (std::function<void (const Change&)> listener)
{
    listeners.emplace_back (nextListenerId, std::move (listener));
    return nextListenerId++;
}

void CommandBus::removeListener (int listenerId)
{
    std::erase_if (listeners, [listenerId] (const auto& listener) { return listener.first == listenerId; });
}

void CommandBus::setTransport (Transport* newTransport)
{
    transport = newTransport;
}

void CommandBus::notify (const Change& change)
{
    for (const auto& listener : listeners)
        listener.second (change);
}
}
