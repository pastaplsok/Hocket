#include "core/commands/CommandBus.h"
#include "core/commands/Planning.h"
#include "core/project/Rules.h"

#include <algorithm>
#include <map>

namespace hocket::commands
{
namespace
{
    constexpr std::size_t maxTransactionNameLength = 100;

    std::string doubleQuoted (std::string_view text)
    {
        return "\"" + std::string (text) + "\"";
    }

    template <typename... Types>
    struct Overloaded : Types...
    {
        using Types::operator()...;
    };
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
void CommandBus::Edits::append (Edits&& more)
{
    forward.insert (forward.end(), std::make_move_iterator (more.forward.begin()), std::make_move_iterator (more.forward.end()));
    inverse.insert (inverse.end(), std::make_move_iterator (more.inverse.begin()), std::make_move_iterator (more.inverse.end()));
    rewritten.merge (more.rewritten);
}

CommandBus::CommandBus (project::Project project) : currentProject (std::move (project))
{
}

Result CommandBus::execute (const Command& command, const Author& author)
{
    if (author.name.empty())
        return { false, "commands need an author, e.g. \"Lee\" or \"Claude\"" };

    if (std::holds_alternative<Play> (command) || std::holds_alternative<Stop> (command))
        return runTransportCommand (command);

    auto planned = plan (command);

    // Invalid, or valid but changes nothing.
    if (! planned.result.ok || planned.edits.empty())
        return planned.result;

    // Another author's command never waits for an open transaction (SPEC §3.1).
    if (transaction && transaction->author != author)
        closeTransactionPart();

    auto applied = applyCommand (std::move (planned.edits));
    const auto remarks = planned.remarks.empty() ? std::string() : ". " + planned.remarks;

    // Undone entries assumed the project as it was; after a new edit they can't be redone.
    undone.clear();

    if (transaction && transaction->author == author)
    {
        transaction->edits.append (std::move (applied));
        return { true, planned.description + " in your open transaction " + doubleQuoted (transaction->name)
                           + "; it becomes a history entry when you commit" + remarks };
    }

    const auto id = nextEntryId;
    record (author, planned.description, std::move (applied));
    return { true, "#" + std::to_string (id) + " " + planned.description + remarks };
}

Result CommandBus::runTransportCommand (const Command& command)
{
    const auto name = std::holds_alternative<Play> (command) ? "play" : "stop";

    if (transport == nullptr)
        return { false, std::string (name) + ": there's no audio engine running, so playback can't be started or stopped here" };

    return std::holds_alternative<Play> (command) ? transport->play() : transport->stop();
}

edits::Plan CommandBus::plan (const Command& command) const
{
    return std::visit (Overloaded {
                           [this] (const WriteClip& c) { return planning::plan (currentProject, c); },
                           [] (const Play&) { return edits::Plan {}; }, // transport commands never get here
                           [] (const Stop&) { return edits::Plan {}; },
                       },
                       command);
}

//==============================================================================
CommandBus::Edits CommandBus::applyCommand (std::vector<Edit> forward)
{
    // The clip files whose text the edits could change, and that text now.
    edits::Files candidates;

    for (const auto& edit : forward)
        edits::addFilesAffected (currentProject, edit, candidates);

    std::map<std::string, std::optional<std::string>, std::less<>> before;

    for (const auto& file : candidates)
        before[file] = project::clipFileText (currentProject, file);

    Edits applied;
    std::vector<Change> changes;

    for (const auto& edit : forward)
        applied.inverse.push_back (edits::apply (currentProject, edit, changes));

    applied.forward = std::move (forward);

    for (const auto& edit : applied.forward)
        edits::addFilesAffected (currentProject, edit, candidates);

    for (const auto& file : candidates)
    {
        const auto was = before.find (file);

        if ((was != before.end() ? was->second : std::nullopt) != project::clipFileText (currentProject, file))
            applied.rewritten.insert (file);
    }

    notify (changes);
    return applied;
}

void CommandBus::applyForward (const Edits& entryEdits)
{
    std::vector<Change> changes;

    for (const auto& edit : entryEdits.forward)
        edits::apply (currentProject, edit, changes);

    notify (changes);
}

void CommandBus::applyInverse (const Edits& entryEdits)
{
    std::vector<Change> changes;

    for (auto i = entryEdits.inverse.size(); i-- > 0;)
        edits::apply (currentProject, entryEdits.inverse[i], changes);

    notify (changes);
}

void CommandBus::record (const Author& author, std::string description, Edits edits)
{
    done.push_back ({ { nextEntryId++, author, std::move (description) }, std::move (edits) });
    undone.clear();
}

void CommandBus::closeTransactionPart()
{
    if (! transaction || transaction->edits.empty())
        return;

    record (transaction->author, transaction->name + " (part " + std::to_string (transaction->part) + ")", std::exchange (transaction->edits, {}));
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
    if (! project::isOneLineText (name, maxTransactionNameLength))
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

    if (finished.edits.empty())
        return { true, finished.part == 1 ? "committed " + doubleQuoted (finished.name) + " with no changes; nothing was added to the history"
                                          : "committed " + doubleQuoted (finished.name) + "; its earlier parts are already in the history" };

    const auto name = finished.part == 1 ? finished.name : finished.name + " (part " + std::to_string (finished.part) + ")";
    const auto id = nextEntryId;
    record (finished.author, name, std::move (finished.edits));
    return { true, "#" + std::to_string (id) + " " + name };
}

Result CommandBus::interruptTransaction (const Author& author)
{
    if (! transaction || transaction->author != author)
        return { false, displayName (author) + " has no open transaction to interrupt" };

    auto finished = std::move (*transaction);
    transaction.reset();

    const auto closed = "transaction " + doubleQuoted (finished.name) + " was closed";

    if (finished.edits.empty())
        return { true, closed + (finished.part == 1 ? " with no edits, so nothing was added to the history"
                                                    : "; its earlier parts are already in the history, and the last part had no edits") };

    const auto name = finished.part == 1 ? finished.name + " (interrupted)"
                                         : finished.name + " (part " + std::to_string (finished.part) + ", interrupted)";
    const auto id = nextEntryId;
    record (finished.author, name, std::move (finished.edits));

    return { true, closed + (finished.part == 1 ? "; its edits are " : "; its earlier parts are already in the history, and the last part is ")
                       + "#" + std::to_string (id) + " " + doubleQuoted (name) };
}

Result CommandBus::rollbackTransaction (const Author& author)
{
    if (! transaction || transaction->author != author)
        return { false, "you don't have an open transaction to roll back" };

    auto finished = std::move (*transaction);
    transaction.reset();

    applyInverse (finished.edits);

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

        if (scope == UndoScope::callersOwn && ! transaction->edits.empty())
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

    applyInverse (entry.edits);

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

    applyForward (entry.edits);

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

std::vector<HistoryEntry> CommandBus::changesTo (std::string_view file) const
{
    std::vector<HistoryEntry> entries;

    for (auto i = done.size(); i-- > 0;)
        if (done[i].edits.rewritten.contains (file))
            entries.push_back (done[i].info);

    return entries;
}

std::optional<OpenTransaction> CommandBus::openTransaction() const
{
    if (! transaction)
        return {};

    return OpenTransaction { transaction->author, transaction->name, transaction->part, ! transaction->edits.empty() };
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

std::optional<bool> CommandBus::isPlaying() const
{
    if (transport == nullptr)
        return {};

    return transport->isPlaying();
}

void CommandBus::notify (const std::vector<Change>& changes)
{
    for (const auto& change : changes)
        for (const auto& listener : listeners)
            listener.second (change);
}
}
