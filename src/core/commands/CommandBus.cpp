#include "core/commands/CommandBus.h"
#include "core/commands/Planning.h"
#include "core/notes/NotesSyntax.h"
#include "core/project/Rules.h"

#include <algorithm>
#include <map>

namespace hocket::commands
{
namespace
{
    constexpr std::size_t maxTransactionNameLength = 100;
    constexpr std::size_t maxSaveNameLength = 100;

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

    if (isTransportCommand (command))
        return runTransportCommand (command);

    auto planned = plan (currentProject, command);

    // Invalid, or valid but changes nothing.
    if (! planned.result.ok || planned.edits.empty())
        return planned.result;

    // Another author's command never waits for an open transaction (SPEC §3.1).
    if (transaction && transaction->author != author)
        closeTransactionPart();

    const auto wasPlaying = playingBeforeChange();
    auto applied = applyCommand (std::move (planned.edits));
    const auto playback = playbackAfterChange (wasPlaying);
    const auto remarks = (planned.remarks.empty() ? std::string() : ". " + planned.remarks) + (playback.empty() ? std::string() : ". " + playback);

    // Undone entries assumed the project as it was; after a new edit they can't be redone.
    undone.clear();

    if (transaction && transaction->author == author)
    {
        // A transaction is one entry anyway: a gesture in it just adds to it.
        transaction->edits.append (std::move (applied));
        transaction->revision = nextRevision++;
        gesture.reset();
        return { true, planned.description + " in your open transaction " + doubleQuoted (transaction->name)
                           + "; it becomes a history entry when you commit" + remarks };
    }

    // The next step of a gesture: the same ID, author and setting as the
    // newest entry's, with nothing in between (SPEC §3.1).
    const auto gestureId = std::visit ([] (const auto& c) -> std::string
    {
        if constexpr (requires { c.gesture; })
            return c.gesture;
        else
            return {};
    }, command);

    std::optional<Gesture> step;

    if (! gestureId.empty() && ! planned.setting.empty())
        step = Gesture { gestureId, author, planned.setting, 0 };

    if (step && gesture && gesture->id == step->id && gesture->author == step->author && gesture->setting == step->setting
        && ! done.empty() && done.back().info.id == gesture->entryId)
    {
        // The entry undoes to the value before the gesture's first step, and
        // redoes to its latest.
        auto& entry = done.back();
        entry.edits.forward = std::move (applied.forward);
        entry.edits.rewritten.merge (applied.rewritten);

        if (entry.edits.forward == entry.edits.inverse)
        {
            const auto id = entry.info.id;
            done.pop_back();
            gesture.reset();
            return { true, planned.description + "; that's where this gesture started, so it leaves no history entry, and #" + std::to_string (id)
                               + " is gone" + remarks };
        }

        entry.info.description = planned.description;
        entry.revision = nextRevision++;
        return { true, "#" + std::to_string (entry.info.id) + " " + planned.description + remarks };
    }

    const auto id = nextEntryId;
    record (author, planned.description, std::move (applied));

    if (step)
    {
        step->entryId = id;
        gesture = std::move (step);
    }

    return { true, "#" + std::to_string (id) + " " + planned.description + remarks };
}

Result CommandBus::executeAsOne (const std::vector<Command>& commands, const Author& author)
{
    if (author.name.empty())
        return { false, "commands need an author, e.g. \"Lee\" or \"Claude\"" };

    if (commands.size() == 1)
        return execute (commands.front(), author);

    // Each is planned against the project as the ones before leave it, on a
    // copy, so nothing applies unless all of them can.
    auto scratch = currentProject;
    std::vector<Edit> all;
    std::vector<std::string> descriptions;
    std::vector<std::string> remarks;
    std::vector<std::string> unchanged;

    for (const auto& command : commands)
    {
        if (isTransportCommand (command))
            return { false, "transport commands don't change the project, so they can't be part of a history entry with other commands" };

        auto planned = plan (scratch, command);

        if (! planned.result.ok)
            return planned.result;

        if (planned.edits.empty())
        {
            unchanged.push_back (planned.result.message);
            continue;
        }

        std::vector<Change> ignored;

        for (const auto& edit : planned.edits)
            edits::apply (scratch, edit, ignored);

        all.insert (all.end(), std::make_move_iterator (planned.edits.begin()), std::make_move_iterator (planned.edits.end()));
        descriptions.push_back (std::move (planned.description));

        if (! planned.remarks.empty())
            remarks.push_back (std::move (planned.remarks));
    }

    const auto join = [] (const std::vector<std::string>& parts, std::string_view separator)
    {
        std::string text;

        for (const auto& part : parts)
            text += (text.empty() ? "" : std::string (separator)) + part;

        return text;
    };

    if (all.empty())
        return { true, join (unchanged, "; ") };

    if (transaction && transaction->author != author)
        closeTransactionPart();

    const auto wasPlaying = playingBeforeChange();
    auto applied = applyCommand (std::move (all));
    undone.clear();

    if (auto playback = playbackAfterChange (wasPlaying); ! playback.empty())
        remarks.push_back (std::move (playback));

    const auto description = join (descriptions, "; ");
    const auto remarkText = remarks.empty() ? std::string() : ". " + join (remarks, ". ");

    if (transaction && transaction->author == author)
    {
        transaction->edits.append (std::move (applied));
        transaction->revision = nextRevision++;
        gesture.reset();
        return { true, description + " in your open transaction " + doubleQuoted (transaction->name) + "; it becomes a history entry when you commit"
                           + remarkText };
    }

    const auto id = nextEntryId;
    record (author, description, std::move (applied));
    return { true, "#" + std::to_string (id) + " " + description + remarkText };
}

Result CommandBus::runTransportCommand (const Command& command)
{
    const auto wasPlaying = transport != nullptr && transport->isPlaying();
    auto result = commands::runTransportCommand (command, currentProject, transportState, transport);

    // The output's level counts from each play (SPEC §6.4).
    if (result.ok && std::holds_alternative<Play> (command) && ! wasPlaying && outputMeter != nullptr)
        outputMeter->restart();

    // A wrap a seek or a new loop region made is in its own result.
    if (transport != nullptr)
        transport->takeWrap();

    return result;
}

bool CommandBus::playingBeforeChange()
{
    if (transport == nullptr)
        return false;

    transport->takeWrap();
    return transport->isPlaying();
}

std::string CommandBus::playbackAfterChange (bool wasPlaying)
{
    if (transport == nullptr || ! wasPlaying)
        return {};

    const auto range = playRange (currentProject, transportState);
    const auto position = [this] (notes::Ticks ticks) { return notes::formatPosition (ticks, currentProject.meter); };

    if (const auto wrap = transport->takeWrap())
        return "The song now ends at " + position (range.end) + ", before where the playhead was, so playback wrapped from " + position (wrap->from)
             + " to " + position (wrap->to);

    if (! transportState.looping && transport->isPlaying() && transport->playheadPosition() >= range.end)
        return "The song now ends at " + position (range.end) + ", before the playhead, and loop is off, so playback stops there";

    return {};
}

edits::Plan CommandBus::plan (const project::Project& project, const Command& command) const
{
    return std::visit (Overloaded {
                           [this, &project] (const auto& c)
                           {
                               if constexpr (requires { planning::plan (project, c, kitCatalog); })
                                   return planning::plan (project, c, kitCatalog);
                               else
                                   return planning::plan (project, c);
                           },
                           [this, &project] (const Revert& c) { return planRevert (project, c); },

                           // Transport commands never get here.
                           [] (const Play&) { return edits::Plan {}; },
                           [] (const Stop&) { return edits::Plan {}; },
                           [] (const Seek&) { return edits::Plan {}; },
                           [] (const SetLoopRegion&) { return edits::Plan {}; },
                           [] (const ClearLoopRegion&) { return edits::Plan {}; },
                           [] (const SetLooping&) { return edits::Plan {}; },
                       },
                       command);
}

edits::Plan CommandBus::planRevert (const project::Project& project, const Revert& command) const
{
    using planning::refused;

    if (storage == nullptr)
        return refused ("revert: this session has no project folder, so there are no kept saves to go back to");

    const auto saves = storage->keptSaves();
    const auto found = std::ranges::find (saves, command.save, &KeptSave::number);

    if (found == saves.end())
    {
        if (saves.empty())
            return refused ("revert: there are no kept saves yet. Each save keeps the version it replaces, so there's one to go back to after the next save");

        std::vector<std::string> numbers;

        for (const auto& save : saves)
            numbers.push_back ("#" + std::to_string (save.number));

        return refused ("revert: there's no save #" + std::to_string (command.save) + "; the kept saves are " + planning::listText (numbers));
    }

    auto loaded = storage->loadKeptSave (*found);

    if (! loaded.project)
    {
        std::vector<std::string> problems;

        for (std::size_t i = 0; i < loaded.errors.size() && i < 3; ++i)
            problems.push_back (project::formatDiagnostic (loaded.errors[i]));

        if (loaded.errors.size() > 3)
            problems.push_back ("and " + planning::countText (loaded.errors.size() - 3, "more problem"));

        auto message = "revert: " + describe (*found) + " can't be read, in " + found->folder + ": ";

        for (std::size_t i = 0; i < problems.size(); ++i)
            message += (i == 0 ? "" : "; ") + problems[i];

        return refused (message);
    }

    if (project::write (*loaded.project) == project::write (project))
        return planning::unchanged ("the project is already as " + describe (*found) + " has it, so nothing changed and no history entry was added");

    return planning::plan (project, ReplaceProject { std::move (*loaded.project), "reverted to " + describe (*found) });
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

CommandBus::Revision CommandBus::revision() const
{
    if (transaction && ! transaction->edits.empty())
        return transaction->revision;

    return done.empty() ? 0 : done.back().revision;
}

void CommandBus::record (const Author& author, std::string description, Edits edits, Revision revision)
{
    done.push_back ({ { nextEntryId++, author, std::move (description) }, std::move (edits), revision != 0 ? revision : nextRevision++ });
    undone.clear();
    gesture.reset(); // any new entry ends a gesture; execute starts one again if this is its first step
}

void CommandBus::closeTransactionPart()
{
    if (! transaction || transaction->edits.empty())
        return;

    // The project doesn't change, so the entry keeps the part's revision.
    record (transaction->author, transaction->name + " (part " + std::to_string (transaction->part) + ")", std::exchange (transaction->edits, {}),
            std::exchange (transaction->revision, 0));
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
    record (finished.author, name, std::move (finished.edits), finished.revision);
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
    record (finished.author, name, std::move (finished.edits), finished.revision);

    return { true, closed + (finished.part == 1 ? "; its edits are " : "; its earlier parts are already in the history, and the last part is ")
                       + "#" + std::to_string (id) + " " + doubleQuoted (name) };
}

Result CommandBus::rollbackTransaction (const Author& author)
{
    if (! transaction || transaction->author != author)
        return { false, "you don't have an open transaction to roll back" };

    auto finished = std::move (*transaction);
    transaction.reset();

    const auto wasPlaying = playingBeforeChange();
    applyInverse (finished.edits);

    auto message = "rolled back " + doubleQuoted (finished.name);

    if (finished.part > 1)
        message += "; its earlier parts were already in the history because other changes came in between, so undo those separately if needed";

    if (const auto playback = playbackAfterChange (wasPlaying); ! playback.empty())
        message += ". " + playback;

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

    const auto wasPlaying = playingBeforeChange();
    applyInverse (entry.edits);
    gesture.reset();

    const auto playback = playbackAfterChange (wasPlaying);
    const auto message = "undid #" + std::to_string (entry.info.id) + " " + doubleQuoted (entry.info.description) + " by " + displayName (entry.info.author)
                       + (playback.empty() ? std::string() : ". " + playback);
    entry.undoneBy = author;
    undone.push_back (std::move (entry));
    return { true, message };
}

Result CommandBus::redo (const Author& author, UndoScope scope)
{
    if (transaction && transaction->author == author)
        return { false, "can't redo while your transaction " + doubleQuoted (transaction->name) + " is open; commit or roll it back first" };

    if (undone.empty())
        return { false, "nothing to redo" };

    if (const auto& top = undone.back(); scope == UndoScope::callersOwn && top.undoneBy != author)
    {
        const auto undoer = nameFor (top.undoneBy, author);
        const auto caller = undoer == displayName (top.undoneBy) ? displayName (author) : "this one";

        return { false, "didn't redo: the most recent undo, of #" + std::to_string (top.info.id) + " " + doubleQuoted (top.info.description)
                            + ", was done by " + undoer + ", not " + caller + ", so redoing it would go against their undo. Leave it, or redo "
                              "with force to redo it anyway" };
    }

    auto entry = std::move (undone.back());
    undone.pop_back();

    const auto wasPlaying = playingBeforeChange();
    applyForward (entry.edits);
    gesture.reset();

    const auto playback = playbackAfterChange (wasPlaying);
    const auto message = "redid #" + std::to_string (entry.info.id) + " " + doubleQuoted (entry.info.description) + " by " + displayName (entry.info.author)
                       + (playback.empty() ? std::string() : ". " + playback);
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

std::vector<HistoryEntry> CommandBus::structureChanges() const
{
    std::vector<HistoryEntry> entries;

    for (auto i = done.size(); i-- > 0;)
        if (std::ranges::any_of (done[i].edits.forward, [] (const Edit& edit) { return ! std::holds_alternative<edits::SetNotes> (edit); }))
            entries.push_back (done[i].info);

    return entries;
}

std::optional<OpenTransaction> CommandBus::openTransaction() const
{
    if (! transaction)
        return {};

    return OpenTransaction { transaction->author, transaction->name, transaction->part, ! transaction->edits.empty() };
}

int CommandBus::addListener (std::function<void (const Change&)> listener, std::function<void()> afterChanges)
{
    listeners.push_back ({ nextListenerId, std::move (listener), std::move (afterChanges) });
    return nextListenerId++;
}

void CommandBus::removeListener (int listenerId)
{
    std::erase_if (listeners, [listenerId] (const Listener& listener) { return listener.id == listenerId; });
}

void CommandBus::setTransport (Transport* newTransport)
{
    transport = newTransport;
}

SaveResult CommandBus::save (const Save& request, const Author& author)
{
    if (author.name.empty())
        return { false, "saving needs an author, e.g. \"Lee\" or \"Claude\"" };

    // It names the save in the list of kept saves, one per line, and the folder it's kept in.
    if (! request.name.empty() && ! project::isOneLineText (request.name, maxSaveNameLength))
        return { false, "didn't save: a save's name is one line of text, 1 to " + std::to_string (maxSaveNameLength)
                            + " bytes with no control characters, e.g. \"before the bridge experiment\"" };

    if (storage == nullptr)
        return { false, "can't save: this session has no project folder to save to" };

    auto result = storage->save (currentProject, request, author);

    if (result.ok)
        savedRevision = revision();

    return result;
}

std::vector<KeptSave> CommandBus::keptSaves() const
{
    return storage != nullptr ? storage->keptSaves() : std::vector<KeptSave> {};
}

std::optional<bool> CommandBus::isPlaying() const
{
    if (transport == nullptr)
        return {};

    return transport->isPlaying();
}

void CommandBus::notify (const std::vector<Change>& changes)
{
    if (changes.empty())
        return;

    for (const auto& change : changes)
        for (const auto& listener : listeners)
            listener.changed (change);

    for (const auto& listener : listeners)
        if (listener.afterChanges)
            listener.afterChanges();
}
}
