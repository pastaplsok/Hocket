#include "api/Tools.h"
#include "api/TextChanges.h"
#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace hocket::api
{
namespace
{
    using Json = nlohmann::json;
    using commands::Author;
    using commands::CommandBus;

    constexpr int defaultHistoryLimit = 20;
    constexpr int maxHistoryLimit = 100;

    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string doubleQuoted (std::string_view text)
    {
        return "\"" + std::string (text) + "\"";
    }

    std::string countText (std::size_t count, std::string_view noun)
    {
        return std::to_string (count) + " " + std::string (noun) + (count == 1 ? "" : "s");
    }

    // As in .notes: "1bar", "4bars"
    std::string lengthText (int bars)
    {
        return std::to_string (bars) + (bars == 1 ? "bar" : "bars");
    }

    std::string tempoText (double tempo)
    {
        char buffer[32];
        const auto end = std::to_chars (buffer, buffer + sizeof (buffer), tempo).ptr;
        return { buffer, end };
    }

    ToolResult success (std::string text)
    {
        return { { std::move (text) }, false };
    }

    ToolResult failure (std::string text)
    {
        return { { std::move (text) }, true };
    }

    ToolResult fromBus (const commands::Result& result)
    {
        return { { result.message }, ! result.ok };
    }

    //==============================================================================
    // Arguments are checked here as well as by each tool's inputSchema: clients
    // don't have to enforce the schema, and these errors say how to fix the call.
    enum class Type
    {
        string,
        boolean,
        integer
    };

    struct Parameter
    {
        std::string_view name;
        Type type;
        bool required;
    };

    std::string typeText (Type type)
    {
        switch (type)
        {
            case Type::string:  return "a string";
            case Type::boolean: return "true or false";
            case Type::integer: return "a whole number";
        }

        return {};
    }

    bool hasType (const Json& value, Type type)
    {
        switch (type)
        {
            case Type::string:  return value.is_string();
            case Type::boolean: return value.is_boolean();
            case Type::integer: return value.is_number_integer();
        }

        return false;
    }

    // What's wrong with the arguments, or an empty string.
    std::string checkArguments (std::string_view tool, const Json& arguments, std::initializer_list<Parameter> parameters)
    {
        const auto prefix = std::string (tool) + ": ";
        std::string names;

        for (const auto& parameter : parameters)
            names += (names.empty() ? "" : ", ") + singleQuoted (parameter.name);

        const auto takes = parameters.size() == 0 ? std::string ("it takes no arguments") : "its arguments are " + names;

        if (! arguments.is_object())
            return prefix + "the arguments must be a JSON object; " + takes;

        for (const auto& [key, value] : arguments.items())
        {
            const auto* parameter = std::ranges::find (parameters, key, &Parameter::name);

            if (parameter == parameters.end())
                return prefix + "there's no argument " + singleQuoted (key) + "; " + takes;

            if (! hasType (value, parameter->type))
                return prefix + singleQuoted (key) + " must be " + typeText (parameter->type) + ", not " + value.dump();
        }

        for (const auto& parameter : parameters)
            if (parameter.required && ! arguments.contains (parameter.name))
                return prefix + "missing argument " + singleQuoted (parameter.name) + " (" + typeText (parameter.type) + ")";

        return {};
    }

    //==============================================================================
    struct Call
    {
        CommandBus& bus;
        const Author& caller;
        const Json& arguments;
    };

    ToolResult getProjectOverview (const Call& call)
    {
        if (auto error = checkArguments ("get_project_overview", call.arguments, {}); ! error.empty())
            return failure (error);

        const auto& project = call.bus.project();
        const auto meter = project.meter;
        const auto position = [meter] (notes::Ticks ticks) { return notes::formatPosition (ticks, meter); };
        const auto end = project::songEnd (project);

        auto text = "Tempo " + tempoText (project.tempo) + " BPM, " + std::to_string (meter.beatsPerBar) + "/4. The song is "
                  + countText (static_cast<std::size_t> (end / meter.ticksPerBar()), "bar") + " long: it plays from "
                  + position (0) + " to " + position (end) + " on a loop.\n\nTracks, in display order:\n";

        if (project.tracks.empty())
            text += "- none yet\n";

        for (const auto& track : project.tracks)
        {
            text += "- " + track.id + " " + doubleQuoted (track.name) + " (" + project::trackTypeName (track.type) + "), devices: ";

            for (std::size_t i = 0; i < track.devices.size(); ++i)
                text += (i > 0 ? ", " : "") + track.devices[i].id + " (" + project::deviceTypeName (track.devices[i].type) + ")";

            text += track.devices.empty() ? "none\n" : "\n";

            if (track.clips.empty())
                text += "  - no clips\n";

            for (const auto& clip : track.clips)
                text += "  - clip " + clip.id + ": " + position (clip.start) + " to " + position (project::clipEnd (clip, meter))
                      + " (@length " + lengthText (clip.content.lengthInBars) + "), " + countText (clip.content.notes.size(), "note")
                      + ", file " + clip.file + "\n";
        }

        const auto history = call.bus.history();

        if (history.empty())
            text += "\nHistory: empty; nothing has changed since Hocket opened the project.\n";
        else
            text += "\nHistory: " + (history.size() == 1 ? std::string ("1 entry") : std::to_string (history.size()) + " entries")
                  + "; the latest is #" + std::to_string (history.front().id) + " " + doubleQuoted (history.front().description)
                  + " by " + commands::nameFor (history.front().author, call.caller) + ".\n";

        if (const auto open = call.bus.openTransaction())
            text += "Open transaction: " + doubleQuoted (open->name) + " by " + commands::nameFor (open->author, call.caller)
                  + (open->part > 1 ? ", part " + std::to_string (open->part) : "")
                  + (open->hasEdits ? ", with edits not yet in the history.\n" : ", with no edits yet.\n");
        else
            text += "No transaction is open.\n";

        return success (text);
    }

    ToolResult readClip (const Call& call)
    {
        if (auto error = checkArguments ("read_clip", call.arguments, { { "clip_id", Type::string, true } }); ! error.empty())
            return failure (error);

        const auto& project = call.bus.project();
        const auto clipId = call.arguments.at ("clip_id").get<std::string>();
        const project::Track* track = nullptr;
        const auto* clip = project::findClip (project, clipId, &track);

        if (clip == nullptr)
            return failure ("read_clip: " + project::unknownClipMessage (project, clipId, "read_clip"));

        const auto details = "clip " + clip->id + " on track " + track->id + " " + doubleQuoted (track->name) + "\n"
                           + "file: " + clip->file + "\n"
                           + "from " + notes::formatPosition (clip->start, project.meter) + " to "
                           + notes::formatPosition (project::clipEnd (*clip, project.meter), project.meter)
                           + " (@length " + lengthText (clip->content.lengthInBars) + "), " + countText (clip->content.notes.size(), "note") + "\n"
                           + "The next block is the clip's .notes text, exactly as stored. To change the clip, "
                             "edit that text and send all of it to write_clip.";

        return { { details, notes::write (clip->content, project.meter) }, false };
    }

    ToolResult writeClip (const Call& call)
    {
        if (auto error = checkArguments ("write_clip", call.arguments, { { "clip_id", Type::string, true }, { "notes", Type::string, true } });
            ! error.empty())
            return failure (error);

        const auto clipId = call.arguments.at ("clip_id").get<std::string>();
        const auto sent = call.arguments.at ("notes").get<std::string>();
        const auto result = call.bus.execute (commands::WriteClip { clipId, sent }, call.caller);

        if (! result.ok)
            return failure (result.message);

        const auto& project = call.bus.project();
        const auto stored = notes::write (project::findClip (project, clipId)->content, project.meter);
        const auto changes = describeChanges (sent, stored);

        return success (result.message + "\n"
                        + (changes.empty() ? "Stored exactly as you sent it."
                                           : "Stored in canonical form, which differs from what you sent: " + changes + ". read_clip returns the stored text."));
    }

    ToolResult undo (const Call& call)
    {
        if (auto error = checkArguments ("undo", call.arguments, { { "force", Type::boolean, false } }); ! error.empty())
            return failure (error);

        const auto force = call.arguments.value ("force", false);
        return fromBus (call.bus.undo (call.caller, force ? commands::UndoScope::anyAuthor : commands::UndoScope::callersOwn));
    }

    ToolResult history (const Call& call)
    {
        if (auto error = checkArguments ("history", call.arguments, { { "limit", Type::integer, false } }); ! error.empty())
            return failure (error);

        const auto limit = call.arguments.value ("limit", std::int64_t { defaultHistoryLimit });

        if (limit < 1 || limit > maxHistoryLimit)
            return failure ("history: 'limit' must be from 1 to " + std::to_string (maxHistoryLimit) + ", not " + std::to_string (limit));

        const auto entries = call.bus.history();
        const auto shown = std::min (entries.size(), static_cast<std::size_t> (limit));
        std::string text;

        if (entries.empty())
            text = "The history is empty: nothing has changed since Hocket opened the project.";
        else if (shown == entries.size())
            text = entries.size() == 1 ? "The only entry:" : "Newest first, all " + std::to_string (entries.size()) + " entries:";
        else
            text = "Newest first, the latest " + std::to_string (shown) + " of " + std::to_string (entries.size()) + " entries:";

        for (std::size_t i = 0; i < shown; ++i)
            text += "\n#" + std::to_string (entries[i].id) + " " + commands::nameFor (entries[i].author, call.caller) + ": " + entries[i].description;

        if (const auto open = call.bus.openTransaction(); open && open->hasEdits)
            text += "\nNot listed yet: the edits in the open transaction " + doubleQuoted (open->name) + " by "
                  + commands::nameFor (open->author, call.caller) + ", until it's committed.";

        return success (text);
    }

    ToolResult beginTransaction (const Call& call)
    {
        if (auto error = checkArguments ("begin_transaction", call.arguments, { { "name", Type::string, true } }); ! error.empty())
            return failure (error);

        return fromBus (call.bus.beginTransaction (call.caller, call.arguments.at ("name").get<std::string>()));
    }

    ToolResult commit (const Call& call)
    {
        if (auto error = checkArguments ("commit", call.arguments, {}); ! error.empty())
            return failure (error);

        return fromBus (call.bus.commitTransaction (call.caller));
    }

    ToolResult rollback (const Call& call)
    {
        if (auto error = checkArguments ("rollback", call.arguments, {}); ! error.empty())
            return failure (error);

        return fromBus (call.bus.rollbackTransaction (call.caller));
    }

    ToolResult transport (const Call& call)
    {
        if (auto error = checkArguments ("transport", call.arguments, { { "action", Type::string, true } }); ! error.empty())
            return failure (error);

        const auto action = call.arguments.at ("action").get<std::string>();

        if (action == "play")
            return fromBus (call.bus.execute (commands::Play {}, call.caller));

        if (action == "stop")
            return fromBus (call.bus.execute (commands::Stop {}, call.caller));

        return failure ("transport: 'action' must be \"play\" or \"stop\", not " + doubleQuoted (action));
    }

    //==============================================================================
    struct Tool
    {
        std::string_view name;
        bool readOnly;
        std::string_view description; // "{idle}" stands for the idle timeout
        std::string_view inputSchema; // JSON
        ToolResult (*run) (const Call&);
    };

    constexpr std::string_view noArguments = R"({ "type": "object", "properties": {}, "additionalProperties": false })";

    constexpr std::array tools {
        Tool { "get_project_overview", true,
               "Summarizes the project open in Hocket: tempo, time signature and song length; every track with its devices "
               "and clips (IDs, positions, lengths, note counts); the latest history entry; and any open transaction. "
               "Start here: the other tools take the IDs it lists.",
               noArguments,
               getProjectOverview },

        Tool { "read_clip", true,
               "Returns a clip's notes as .notes text, exactly as Hocket stores them, in a block of its own. The block before "
               "it gives the clip's ID, track, file, position and length. To change a clip, edit that text and send all of it "
               "back with write_clip.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string", "description": "The clip's ID, e.g. \"clp-bass\". get_project_overview lists them." }
                    },
                    "required": [ "clip_id" ],
                    "additionalProperties": false
                  })",
               readClip },

        Tool { "write_clip", false,
               "Replaces all of a clip's notes with new .notes text. If Hocket is playing, the change is heard on the next pass "
               "of the loop. The text is checked first: if anything is wrong, nothing changes and the result lists every "
               "problem as line:column. The result names the new history entry, and says whether Hocket stored the text "
               "exactly as sent or reformatted it to canonical form.\n"
               "\n"
               ".notes format: an @length line (\"@length 2bars\"), then one note per line: position, pitch, length, "
               "velocity, separated by spaces, e.g. \"1.3.3 Eb2 1/8 v95\".\n"
               "- position: bar.beat[.sixteenth][+ticks], 1-based and relative to the clip start. 1.1 is the first beat; "
               "2.3.3 is bar 2, beat 3, third sixteenth; +ticks adds 1/960ths of a beat.\n"
               "- pitch: note name and octave, e.g. C2, F#4, Bb3 (C4 is middle C). A chord is [C3 E3 G3].\n"
               "- length: a fraction of a whole note (1/4, 1/8, 3/16), a triplet (1/8t), or ticks (250tk).\n"
               "- velocity: v1 to v127, required on every line.\n"
               "- # starts a comment, kept as written.\n"
               "Only 4/4 for now. Not supported yet: cc, pb, aftertouch, @swing, @ch.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string", "description": "The clip's ID, e.g. \"clp-bass\"." },
                      "notes": { "type": "string", "description": "The clip's complete new .notes text." }
                    },
                    "required": [ "clip_id", "notes" ],
                    "additionalProperties": false
                  })",
               writeClip },

        Tool { "undo", false,
               "Takes back the most recent change in the project's history. Everyone working in Hocket shares one history, "
               "so the most recent change may not be yours: then undo refuses and says who made it and what it was. Pass "
               "force only when the user has asked you to undo someone else's change. While your own transaction is open, "
               "use rollback instead.",
               R"({
                    "type": "object",
                    "properties": {
                      "force": { "type": "boolean", "default": false, "description": "Undo the most recent change even if someone else made it." }
                    },
                    "additionalProperties": false
                  })",
               undo },

        Tool { "history", true,
               "Lists the most recent changes, newest first: each entry's number, who made it (you, the user, or another "
               "client) and what it did. Undo always takes back the newest entry.",
               R"({
                    "type": "object",
                    "properties": {
                      "limit": { "type": "integer", "minimum": 1, "maximum": 100, "default": 20, "description": "How many entries to list." }
                    },
                    "additionalProperties": false
                  })",
               history },

        Tool { "begin_transaction", false,
               "Groups your next edits into one history entry, named for what they do together (e.g. \"wrote bridge\"), so "
               "they can be undone in one step. Finish with commit, or use rollback to take them all back. Only one "
               "transaction can be open at a time in Hocket, but nobody waits for yours: if someone else edits meanwhile, "
               "your edits so far become their own entry (\"wrote bridge (part 1)\") and the transaction carries on. If you "
               "disconnect, or send no requests for {idle}, Hocket commits it as \"wrote bridge (interrupted)\".",
               R"({
                    "type": "object",
                    "properties": {
                      "name": { "type": "string", "description": "What the edits do together, e.g. \"wrote bridge\". It becomes the history entry's name." }
                    },
                    "required": [ "name" ],
                    "additionalProperties": false
                  })",
               beginTransaction },

        Tool { "commit", false,
               "Ends your open transaction and adds its edits to the history as one entry, named after the transaction.",
               noArguments,
               commit },

        Tool { "rollback", false,
               "Ends your open transaction and takes back its edits. If someone else's edit split the transaction, only the "
               "edits since then are taken back; the earlier part is already in the history.",
               noArguments,
               rollback },

        Tool { "transport", false,
               "Starts or stops playback. Play loops the whole song, from 1.1 to the end of the last clip.",
               R"({
                    "type": "object",
                    "properties": {
                      "action": { "type": "string", "enum": [ "play", "stop" ] }
                    },
                    "required": [ "action" ],
                    "additionalProperties": false
                  })",
               transport },
    };

    const Tool* findTool (std::string_view name)
    {
        const auto tool = std::ranges::find (tools, name, &Tool::name);
        return tool == tools.end() ? nullptr : &*tool;
    }

    std::string replaceAll (std::string text, std::string_view from, std::string_view to)
    {
        for (auto at = text.find (from); at != std::string::npos; at = text.find (from, at + to.size()))
            text.replace (at, from.size(), to);

        return text;
    }
}

Json toolDefinitions (std::chrono::seconds idleTimeout)
{
    auto definitions = Json::array();

    for (const auto& tool : tools)
    {
        definitions.push_back ({
            { "name", std::string (tool.name) },
            { "description", replaceAll (std::string (tool.description), "{idle}", durationText (idleTimeout)) },
            { "inputSchema", Json::parse (tool.inputSchema) },
            { "annotations", { { "readOnlyHint", tool.readOnly } } },
        });
    }

    return definitions;
}

std::optional<ToolResult> callTool (CommandBus& bus, const Author& caller, std::string_view name, const Json& arguments)
{
    const auto* tool = findTool (name);

    if (tool == nullptr)
        return {};

    return tool->run ({ bus, caller, arguments });
}

bool isReadOnlyTool (std::string_view name)
{
    const auto* tool = findTool (name);
    return tool != nullptr && tool->readOnly;
}

std::string durationText (std::chrono::seconds duration)
{
    const auto seconds = duration.count();

    if (seconds % 60 != 0)
        return countText (static_cast<std::size_t> (seconds), "second");

    return countText (static_cast<std::size_t> (seconds / 60), "minute");
}
}
