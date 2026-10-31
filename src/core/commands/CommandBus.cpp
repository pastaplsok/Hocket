#include "core/commands/CommandBus.h"
#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <cassert>

namespace hocket::commands
{
namespace
{
    constexpr std::size_t maxTransactionNameLength = 100;

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

    project::Clip& clipById (project::Project& project, std::string_view id)
    {
        auto* clip = const_cast<project::Clip*> (project::findClip (project, id));
        assert (clip != nullptr); // edits are only made for clips that validation found
        return *clip;
    }
}

std::string displayName (const Author& author)
{
    return author.label.empty() ? author.name : author.name + " (" + author.label + ")";
}

std::string nameFor (const Author& author, const Author& reader)
{
    if (author == reader)
        return "you";

    const auto name = displayName (author);
    return name == displayName (reader) ? name + " in another session" : name;
}

//==============================================================================
CommandBus::CommandBus (project::Project project) : currentProject (std::move (project))
{
}

Result CommandBus::execute (const Command& command, const Author& author)
{
    if (author.name.empty())
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
        return { true, description + " in your open transaction " + doubleQuoted (transaction->name)
                           + "; it becomes a history entry when you commit" };
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
    const auto* clip = project::findClip (currentProject, command.clipId, &track);

    if (clip == nullptr)
        return { false, "write_clip: " + project::unknownClipMessage (currentProject, command.clipId, "write_clip") };

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

    // Compared in canonical form: the parser keeps notes and comments where the
    // text had them, so two texts that write out the same can parse differently,
    // e.g. a client sending back read_clip's text for a clip loaded from looser text.
    if (notes::write (*parsed.clip, meter) == notes::write (clip->content, meter))
        return { true, "clip " + singleQuoted (clip->id) + " already has exactly these notes, so nothing changed and no history entry was added" };

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

void CommandBus::record (const Author& author, std::string description, std::vector<Edit> forward, std::vector<Edit> inverse)
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
Result CommandBus::beginTransaction (const Author& author, const std::string& name)
{
    if (author.name.empty())
        return { false, "transactions need an author, e.g. \"Lee\" or \"Claude\"" };

    if (name.empty())
        return { false, "a transaction needs a name that says what it does, e.g. \"wrote bridge\"; it becomes the undo step's name" };

    // The name goes into the history, which tools list one entry per line, and the log.
    const auto hasControlCharacter = std::ranges::any_of (name, [] (unsigned char c) { return c < 0x20 || c == 0x7f; });

    if (hasControlCharacter || name.size() > maxTransactionNameLength)
        return { false, "a transaction's name is one line of text, 1 to " + std::to_string (maxTransactionNameLength)
                            + " bytes with no control characters, e.g. \"wrote bridge\"; it becomes the history entry's name" };

    if (transaction)
    {
        if (transaction->author == author)
            return { false, "you already have an open transaction " + doubleQuoted (transaction->name) + "; commit or roll it back first" };

        return { false, nameFor (transaction->author, author) + " has an open transaction " + doubleQuoted (transaction->name)
                            + ", and only one can be open at a time. Try again once it's committed or rolled back; "
                              "single commands don't need to wait" };
    }

    transaction = Transaction { author, name };
    return { true, "began transaction " + doubleQuoted (name) };
}

Result CommandBus::commitTransaction (const Author& author)
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

Result CommandBus::interruptTransaction (const Author& author)
{
    if (! transaction || transaction->author != author)
        return { false, displayName (author) + " has no open transaction to interrupt" };

    auto finished = std::move (*transaction);
    transaction.reset();

    const auto closed = "transaction " + doubleQuoted (finished.name) + " was closed";

    if (finished.forward.empty())
        return { true, closed + (finished.part == 1 ? " with no edits, so nothing was added to the history"
                                                    : "; its earlier parts are already in the history, and the last part had no edits") };

    const auto name = finished.part == 1 ? finished.name + " (interrupted)"
                                         : finished.name + " (part " + std::to_string (finished.part) + ", interrupted)";
    const auto id = nextEntryId;
    record (finished.author, name, std::move (finished.forward), std::move (finished.inverse));

    return { true, closed + (finished.part == 1 ? "; its edits are " : "; its earlier parts are already in the history, and the last part is ")
                       + "#" + std::to_string (id) + " " + doubleQuoted (name) };
}

Result CommandBus::rollbackTransaction (const Author& author)
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
Result CommandBus::undo (const Author& author, UndoScope scope)
{
    if (transaction)
    {
        if (transaction->author == author)
            return { false, "can't undo while your transaction " + doubleQuoted (transaction->name) + " is open; roll it back instead, or commit it first" };

        if (scope == UndoScope::callersOwn && ! transaction->forward.empty())
            return { false, "didn't undo: the most recent change is part of the open transaction " + doubleQuoted (transaction->name) + " by "
                                + nameFor (transaction->author, author) + ", not yours. Leave it, or undo with force to undo it anyway" };

        closeTransactionPart();
    }

    if (done.empty())
        return { false, "nothing to undo: the history is empty" };

    const auto& top = done.back().info;

    if (scope == UndoScope::callersOwn && top.author != author)
    {
        const auto maker = nameFor (top.author, author);
        const auto caller = maker == displayName (top.author) ? displayName (author) : "this one";

        return { false, "didn't undo: the most recent change, #" + std::to_string (top.id) + " " + doubleQuoted (top.description)
                            + ", was made by " + maker + ", not " + caller + ". Undo only takes back the most recent change, "
                              "so this would undo their work. Leave it, or undo with force to undo it anyway" };
    }

    auto entry = std::move (done.back());
    done.pop_back();

    for (auto i = entry.inverse.size(); i-- > 0;)
        apply (entry.inverse[i]);

    const auto message = "undid #" + std::to_string (entry.info.id) + " " + doubleQuoted (entry.info.description) + " by " + displayName (entry.info.author);
    undone.push_back (std::move (entry));
    return { true, message };
}

Result CommandBus::redo (const Author& author)
{
    if (transaction && transaction->author == author)
        return { false, "can't redo while your transaction " + doubleQuoted (transaction->name) + " is open; commit or roll it back first" };

    if (undone.empty())
        return { false, "nothing to redo" };

    auto entry = std::move (undone.back());
    undone.pop_back();

    for (const auto& edit : entry.forward)
        apply (edit);

    const auto message = "redid #" + std::to_string (entry.info.id) + " " + doubleQuoted (entry.info.description) + " by " + displayName (entry.info.author);
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

std::vector<HistoryEntry> CommandBus::changesTo (std::string_view clipId) const
{
    const auto changesClip = [clipId] (const Edit& edit)
    {
        return std::visit ([clipId] (const SetClipContent& set) { return set.clipId == clipId; }, edit);
    };

    std::vector<HistoryEntry> entries;

    for (auto i = done.size(); i-- > 0;)
        if (std::ranges::any_of (done[i].forward, changesClip))
            entries.push_back (done[i].info);

    return entries;
}

std::optional<OpenTransaction> CommandBus::openTransaction() const
{
    if (! transaction)
        return {};

    return OpenTransaction { transaction->author, transaction->name, transaction->part, ! transaction->forward.empty() };
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
