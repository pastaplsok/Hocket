#include "api/Tools.h"
#include "api/TextChanges.h"
#include "core/notes/NotesPatch.h"
#include "core/notes/NotesSyntax.h"
#include "core/units/Units.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>

namespace hocket::api
{
namespace
{
    using Json = nlohmann::json;
    using commands::Author;
    using commands::CommandBus;

    constexpr int defaultListLimit = 20;
    constexpr int maxListLimit = 100;

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

    std::string joined (const std::vector<std::string>& parts, std::string_view separator)
    {
        std::string text;

        for (const auto& part : parts)
            text += (text.empty() ? "" : std::string (separator)) + part;

        return text;
    }

    std::string trimmed (std::string_view text)
    {
        while (! text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix (1);

        while (! text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
            text.remove_suffix (1);

        return std::string (text);
    }

    std::string lowercase (std::string text)
    {
        for (auto& c : text)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char> (c - 'A' + 'a');

        return text;
    }

    // As in .notes: "1bar", "4bars"
    std::string lengthText (int bars)
    {
        return std::to_string (bars) + (bars == 1 ? "bar" : "bars");
    }

    std::string position (const project::Project& project, notes::Ticks ticks)
    {
        return notes::formatPosition (ticks, project.meter);
    }

    // " (linked with clp-b, clp-c)" for a clip whose file other clips use too; empty otherwise.
    std::string linkedText (const project::Project& project, const project::Clip& clip)
    {
        std::string others;

        for (const auto* other : project::clipsUsing (project, clip.file))
            if (other != &clip)
                others += (others.empty() ? "" : ", ") + other->id;

        return others.empty() ? std::string() : " (linked with " + others + ")";
    }

    // The file a clip's ID implies: clp-verse -> clips/verse.notes (SPEC §6.4).
    std::string impliedFile (const std::string& clipId)
    {
        return "clips/" + (clipId.starts_with ("clp-") ? clipId.substr (4) : clipId) + ".notes";
    }

    //==============================================================================
    // A drum map as the overview shows it, and set_track's drum_map takes it
    // (SPEC §6.4): "standard", "standard, rim=37, -cabasa", or listed whole
    // after "empty" when that's shorter.
    std::string drumMapText (const notes::DrumMap& drumMap)
    {
        const auto& standard = notes::standardDrumMap();
        std::vector<std::string> changes;

        // Names the standard map doesn't have on that note. One on a note the
        // standard map names replaces that name, so the old one needs no "-".
        for (const auto& [note, name] : drumMap.byNote())
            if (const auto* standardName = standard.nameOf (note); standardName == nullptr || *standardName != name)
                changes.push_back (name + "=" + std::to_string (note));

        for (const auto& [note, name] : standard.byNote())
            if (! drumMap.noteOf (name) && drumMap.nameOf (note) == nullptr)
                changes.push_back ("-" + name);

        auto differences = std::string ("standard");

        for (const auto& change : changes)
            differences += ", " + change;

        auto whole = std::string ("empty");

        for (const auto& [note, name] : drumMap.byNote())
            whole += ", " + name + "=" + std::to_string (note);

        return whole.size() < differences.size() ? whole : differences;
    }

    // set_track's drum_map, read: "standard, rim=37, -cabasa".
    std::optional<commands::ChangeDrumMap> parseDrumMapChanges (const std::string& trackId, const std::string& text, std::string& error)
    {
        commands::ChangeDrumMap changes { trackId, commands::ChangeDrumMap::Base::current, {}, {} };
        std::size_t index = 0;

        for (std::size_t start = 0; start <= text.size(); ++index)
        {
            const auto end = std::min (text.find (',', start), text.size());
            const auto item = trimmed (std::string_view (text).substr (start, end - start));
            start = end + 1;

            const auto what = "set_track: drum_map's " + doubleQuoted (item) + " isn't a change; ";
            const std::string how = "write name=note to name a note (\"rim=37\"), -name to remove a name (\"-cabasa\"), and start with standard or empty "
                             "to begin from that map instead of the track's, e.g. \"standard, rim=37, -cabasa\"";

            if (item.empty())
            {
                error = "set_track: drum_map has an empty item; " + how;
                return std::nullopt;
            }

            if (item == "standard" || item == "empty")
            {
                if (index != 0)
                {
                    error = "set_track: drum_map's " + item + " must come first, since it's the map the changes begin from";
                    return std::nullopt;
                }

                changes.base = item == "standard" ? commands::ChangeDrumMap::Base::standard : commands::ChangeDrumMap::Base::empty;
                continue;
            }

            if (item.starts_with ("-"))
            {
                changes.remove.push_back (trimmed (std::string_view (item).substr (1)));
                continue;
            }

            const auto equals = item.find ('=');

            if (equals == std::string::npos)
            {
                error = what + how;
                return std::nullopt;
            }

            const auto name = trimmed (std::string_view (item).substr (0, equals));
            const auto noteText = trimmed (std::string_view (item).substr (equals + 1));
            int note = -1;
            const auto [end2, problem] = std::from_chars (noteText.data(), noteText.data() + noteText.size(), note);

            if (problem != std::errc() || end2 != noteText.data() + noteText.size())
            {
                error = "set_track: drum_map's " + doubleQuoted (item) + " needs a MIDI note number after the =, 0 to 127, e.g. \"rim=37\"";
                return std::nullopt;
            }

            changes.set.push_back ({ name, note });
        }

        return changes;
    }

    //==============================================================================
    // A track's kit, for the overview (SPEC §8.4): where it came from and which
    // of the track's drum map names it plays, or why it plays nothing.
    // "electronic on trk-drums (bundled; Electronic kit): kick, snare, ..."
    std::string kitText (CommandBus& bus, const project::Track& track, const std::string& kitName)
    {
        kits::KitCatalog none;
        auto& catalog = bus.kits() != nullptr ? *bus.kits() : none;
        const auto found = catalog.find (kitName);
        const auto* drumMap = project::drumMapOf (track);

        if (found.kit == nullptr)
            return doubleQuoted (kitName) + " on " + track.id + ": " + found.problem + ", so the track is silent";

        const auto& kit = *found.kit;
        auto text = kit.name + " on " + track.id + " (" + kits::KitCatalog::describeSource (kit) + "; " + kit.manifest.description + "): ";

        std::vector<std::string> plays;

        if (drumMap != nullptr)
            for (const auto& [note, name] : drumMap->byNote())
                if (const auto* piece = kit.manifest.pieceOn (note))
                    plays.push_back (name + (piece->name == name ? "" : " (the kit's " + singleQuoted (piece->name) + ")"));

        if (drumMap == nullptr || drumMap->empty())
            text += "the drum map is empty, so it names none of the kit's pieces";
        else if (plays.empty())
            text += "it plays none of the drum map's names, so the track is silent";
        else
            text += joined (plays, ", ");

        // Pieces the map doesn't name still play, written as note names.
        std::string unnamed;

        for (const auto& piece : kit.manifest.pieces)
            if (drumMap == nullptr || drumMap->nameOf (piece.note) == nullptr)
                unnamed += (unnamed.empty() ? "" : ", ") + notes::formatPitch (piece.note, notes::Accidental::sharp) + " (note "
                         + std::to_string (piece.note) + ", " + singleQuoted (piece.name) + ")";

        if (! unnamed.empty())
            text += ". It also has pieces on notes the drum map doesn't name, written as note names: " + unnamed;

        // What its files on disk, and loading them, say.
        if (! kit.problem.empty())
            text += ". Its files have changed since it loaded, but can't be used as they are, so the version that loaded plays on: " + kit.problem;

        for (std::size_t i = 0; i < kit.loaded.problems.size(); ++i)
            text += (i == 0 ? ". Loading it found: " : "; ") + kit.loaded.problems[i];

        if (! kit.loaded.overFullScale.empty())
        {
            text += ". " + std::string (kit.loaded.overFullScale.size() == 1 ? "A piece's sample peaks" : "Pieces' samples peak") + " above full scale: ";

            for (std::size_t i = 0; i < kit.loaded.overFullScale.size(); ++i)
            {
                char peak[16];
                std::snprintf (peak, sizeof (peak), "%+.2f", kit.loaded.overFullScale[i].peak);
                text += (i > 0 ? ", " : "") + kit.loaded.overFullScale[i].piece + " at " + peak + " dBFS";
            }
        }

        return text;
    }

    // "Installed kits: electronic (bundled, 23 pieces)."
    std::string installedKitsText (CommandBus& bus)
    {
        std::vector<std::string> kits;

        if (bus.kits() != nullptr)
            for (const auto* kit : bus.kits()->kits())
                kits.push_back (kit->name + " (" + (kit->source == kits::Source::bundled ? std::string ("bundled") : std::string ("the user's"))
                                + ", " + countText (kit->manifest.pieces.size(), "piece") + ")");

        return kits.empty() ? "Installed kits: none, so a drum sampler has nothing to play." : "Installed kits: " + joined (kits, ", ") + ".";
    }

    // "kick (2 notes), A#5 (note 82)": notes by the name the track writes them
    // with, each pitch once, with a count where there's more than one.
    std::string pitchListText (const notes::DrumMap* drumMap, const std::map<int, std::pair<notes::Accidental, std::size_t>>& pitches)
    {
        std::string list;

        for (const auto& [pitch, spellingAndCount] : pitches)
        {
            const auto& [spelling, count] = spellingAndCount;
            const auto* name = drumMap != nullptr ? drumMap->nameOf (pitch) : nullptr;
            list += (list.empty() ? "" : ", ") + (name != nullptr ? *name : notes::formatPitch (pitch, spelling));

            if (name != nullptr && count > 1)
                list += " (" + countText (count, "note") + ")";
            else if (name == nullptr)
                list += " (note " + std::to_string (pitch) + (count > 1 ? ", " + countText (count, "note") : "") + ")";
        }

        return list;
    }

    // On a track whose drum sampler can't play some of a clip's notes, which
    // ones (SPEC §5.2): those its kit has no piece for, or all of them when
    // the kit can't play. Empty otherwise.
    std::string kitGapsText (CommandBus& bus, const project::Track& track, const notes::Clip& content)
    {
        const auto sampler = std::ranges::find (track.devices, project::DeviceType::drumSampler, &project::Device::type);

        if (sampler == track.devices.end() || content.notes.empty())
            return {};

        kits::KitCatalog none;
        auto& catalog = bus.kits() != nullptr ? *bus.kits() : none;
        const auto found = catalog.find (sampler->kit);

        if (found.kit == nullptr)
            return "None of these notes sound: " + found.problem + ", so " + track.id + " is silent.";

        std::map<int, std::pair<notes::Accidental, std::size_t>> silent; // pitch -> spelling, count

        for (const auto& note : content.notes)
            if (found.kit->manifest.pieceOn (note.pitch) == nullptr)
                ++silent.try_emplace (note.pitch, note.spelling, 0).first->second.second;

        if (silent.empty())
            return {};

        return "Kit " + doubleQuoted (found.kit->name) + " has no piece for these, so they're silent: "
             + pitchListText (project::drumMapOf (track), silent) + ".";
    }

    // On a drums track, the notes its drum map doesn't name (SPEC §5.2): they're
    // written as note names. Empty otherwise.
    std::string unnamedNotesText (const project::Track& track, const notes::Clip& content)
    {
        const auto* drumMap = project::drumMapOf (track);

        if (drumMap == nullptr)
            return {};

        std::map<int, std::pair<notes::Accidental, std::size_t>> unnamed; // pitch -> spelling, count

        for (const auto& note : content.notes)
            if (drumMap->nameOf (note.pitch) == nullptr)
                ++unnamed.try_emplace (note.pitch, note.spelling, 0).first->second.second;

        if (unnamed.empty())
            return {};

        return "These notes have no name in " + track.id + "'s drum map, so they're written as note names: " + pitchListText (nullptr, unnamed) + ".";
    }

    // After a clip's notes changed: the drum names and kit gaps, as lines; empty if none.
    std::string clipNotices (CommandBus& bus, const std::string& clipId)
    {
        const auto& project = bus.project();
        const project::Track* track = nullptr;
        const auto* clip = project::findClip (project, clipId, &track);

        if (clip == nullptr)
            return {};

        const auto& content = project::notesOf (project, *clip);
        std::string text;

        for (const auto& line : { unnamedNotesText (*track, content), kitGapsText (bus, *track, content) })
            if (! line.empty())
                text += "\n" + line;

        return text;
    }

    std::string tempoText (double tempo)
    {
        char buffer[32];
        const auto end = std::to_chars (buffer, buffer + sizeof (buffer), tempo).ptr;
        return { buffer, end };
    }

    //==============================================================================
    // The transport's state and the output line (SPEC §6.4).

    // "42 s", "1 min 12 s", "1 h 3 min"
    std::string playedTimeText (double seconds)
    {
        const auto whole = static_cast<long long> (std::llround (std::max (0.0, seconds)));

        if (whole < 60)
            return std::to_string (whole) + " s";

        if (whole < 3600)
            return std::to_string (whole / 60) + " min " + std::to_string (whole % 60) + " s";

        return std::to_string (whole / 3600) + " h " + std::to_string ((whole % 3600) / 60) + " min";
    }

    std::string transportLine (const CommandBus& bus)
    {
        const auto playing = bus.isPlaying();

        if (! playing)
            return "Transport: there's no audio engine here, so it can't play.";

        const auto& project = bus.project();
        const auto& settings = bus.transportSettings();
        const auto range = commands::playRange (project, settings);
        const auto what = std::string (settings.loopRegion ? "the loop region " : "the whole song, ") + position (project, range.start) + " to "
                        + position (project, range.end) + (settings.looping ? ", on a loop" : ", once (loop is off)");

        if (*playing)
            return "Transport: playing " + what + "; the playhead is at " + position (project, bus.currentTransport()->playheadPosition()) + ".";

        return "Transport: stopped. Play plays " + what + ", from " + position (project, commands::startPosition (project, settings)) + ".";
    }

    // Empty without a transport.
    std::string outputLine (const CommandBus& bus)
    {
        const auto* transport = bus.currentTransport();

        if (transport == nullptr)
            return {};

        const auto progress = transport->playProgress();

        if (! progress.started)
            return "Output: nothing has played since Hocket started.";

        auto text = std::string (transport->isPlaying() ? "Output since play started: " : "Output while it last played: ") + playedTimeText (progress.seconds);
        const auto& settings = bus.transportSettings();

        if (const auto length = commands::playRange (bus.project(), settings).length(); settings.looping && length > 0)
        {
            const auto passes = progress.ticks / length;
            text += ", " + std::to_string (passes) + (passes == 1 ? " complete pass" : " complete passes");
        }

        const auto* meter = bus.meter();

        if (meter == nullptr)
            return text + "; there's no output meter here.";

        const auto level = meter->levelSincePlay();

        if (level.peak <= 0.0f)
            return text + "; silent.";

        char peak[32];
        std::snprintf (peak, sizeof (peak), "%+.1f dBFS", 20.0 * std::log10 (static_cast<double> (level.peak)));
        text += "; peak " + std::string (peak);

        return text + (level.clippedSamples == 0 ? ", never over full scale."
                                                 : ", " + countText (static_cast<std::size_t> (level.clippedSamples), "sample")
                                                       + " over full scale, so the mix clips.");
    }

    //==============================================================================
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
        integer,
        number,
        array
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
            case Type::number:  return "a number";
            case Type::array:   return "an array";
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
            case Type::number:  return value.is_number();
            case Type::array:   return value.is_array();
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

    std::string stringArgument (const Json& arguments, std::string_view name)
    {
        const auto found = arguments.find (name);
        return found == arguments.end() ? std::string() : found->get<std::string>();
    }

    std::optional<std::string> optionalString (const Json& arguments, std::string_view name)
    {
        const auto found = arguments.find (name);
        return found == arguments.end() ? std::nullopt : std::optional<std::string> (found->get<std::string>());
    }

    std::optional<bool> optionalBool (const Json& arguments, std::string_view name)
    {
        const auto found = arguments.find (name);
        return found == arguments.end() ? std::nullopt : std::optional<bool> (found->get<bool>());
    }

    // `limit`, 1 to maxListLimit, or the default.
    std::optional<std::size_t> listLimit (const Json& arguments, std::string_view tool, std::string& error)
    {
        const auto limit = arguments.value ("limit", std::int64_t { defaultListLimit });

        if (limit < 1 || limit > maxListLimit)
        {
            error = std::string (tool) + ": 'limit' must be from 1 to " + std::to_string (maxListLimit) + ", not " + std::to_string (limit);
            return std::nullopt;
        }

        return static_cast<std::size_t> (limit);
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
        const auto end = project::songEnd (project);

        // In 6/8, a beat (and so a position's second number) is an eighth note, but BPM still counts quarter notes.
        const auto beats = meter.denominator == 4 ? std::string()
                         : " (a beat is " + std::string (meter.denominator == 2 ? "a half" : meter.denominator == 8 ? "an eighth" : "a sixteenth")
                               + " note; BPM counts quarter notes)";

        auto text = "Tempo " + tempoText (project.tempo) + " BPM, " + meter.text() + beats + ". The song is "
                  + countText (static_cast<std::size_t> (end / meter.ticksPerBar()), "bar") + " long, from " + position (project, 0) + " to "
                  + position (project, end) + ".\n" + transportLine (call.bus) + "\n";

        if (const auto output = outputLine (call.bus); ! output.empty())
            text += output + "\n";

        text += "\nTracks, in display order:\n";

        if (project.tracks.empty())
            text += "- none yet\n";

        std::vector<std::string> kitsInUse;

        for (const auto& track : project.tracks)
        {
            text += "- " + track.id + " " + doubleQuoted (track.name) + " (" + project::trackTypeName (track.type) + "; volume "
                  + units::format (track.volume, units::volume()) + ", pan " + units::formatPan (track.pan) + (track.mute ? ", muted" : "")
                  + (track.solo ? ", soloed" : "") + "), devices: ";

            for (std::size_t i = 0; i < track.devices.size(); ++i)
            {
                const auto& device = track.devices[i];
                text += (i > 0 ? ", " : "") + device.id + " (" + project::deviceTypeName (device.type)
                      + (device.type == project::DeviceType::drumSampler ? ", kit " + doubleQuoted (device.kit) : "") + ")";

                if (device.type == project::DeviceType::drumSampler)
                    kitsInUse.push_back (kitText (call.bus, track, device.kit));
            }

            if (track.devices.empty())
                text += "none";

            if (const auto* drumMap = project::drumMapOf (track))
                text += ", drum map: " + drumMapText (*drumMap);

            text += "\n";

            if (track.clips.empty())
                text += "  - no clips\n";

            // In song order, as project.json's canonical form has them (SPEC §4.4).
            std::vector<const project::Clip*> clips;

            for (const auto& clip : track.clips)
                clips.push_back (&clip);

            std::ranges::stable_sort (clips, {}, &project::Clip::start);

            for (const auto* clip : clips)
            {
                const auto& content = project::notesOf (project, *clip);
                const auto linked = linkedText (project, *clip);

                text += "  - clip " + clip->id + ": " + position (project, clip->start) + " to " + position (project, project::clipEnd (project, *clip))
                      + " (@length " + lengthText (content.lengthInBars) + "), " + countText (content.notes.size(), "note")
                      + (linked.empty() && clip->file == impliedFile (clip->id) ? std::string() : ", file " + clip->file + linked) + "\n";
            }
        }

        if (! kitsInUse.empty())
        {
            text += "\nKits in use, with the drum map names each plays on its track (the map's other names are silent):\n";

            for (const auto& kit : kitsInUse)
                text += "- " + kit + "\n";
        }

        text += (kitsInUse.empty() ? "\n" : "") + installedKitsText (call.bus) + "\n";

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
                  + (open->hasEdits ? ", with edits not yet in the history. " : ", with no edits yet. ");
        else
            text += "No transaction is open. ";

        text += call.bus.hasUnsavedChanges() ? "There are unsaved changes; save writes them.\n" : "Nothing is unsaved.\n";
        return success (text);
    }

    // "5-8" or "5": the first and last bar, 1-based.
    std::optional<std::pair<int, int>> parseBars (const std::string& text, int clipBars, std::string& error)
    {
        const auto dash = text.find ('-');
        const auto read = [] (std::string_view part) -> std::optional<int>
        {
            const auto value = trimmed (part);
            int number = 0;
            const auto [end, problem] = std::from_chars (value.data(), value.data() + value.size(), number);
            return problem == std::errc() && end == value.data() + value.size() && ! value.empty() ? std::optional<int> (number) : std::nullopt;
        };

        const auto first = read (std::string_view (text).substr (0, dash));
        const auto last = dash == std::string::npos ? first : read (std::string_view (text).substr (dash + 1));
        const auto clipBarsText = "the clip has " + countText (static_cast<std::size_t> (clipBars), "bar");

        if (! first || ! last)
            error = "read_clip: 'bars' is a bar of the clip, or a range of them, counted as its positions count them: \"5\" or \"5-8\", not "
                  + doubleQuoted (text);
        else if (*first < 1 || *last > clipBars || *first > *last)
            error = "read_clip: 'bars' " + doubleQuoted (text) + " isn't in the clip: " + clipBarsText + ", so bars run from 1 to "
                  + std::to_string (clipBars);
        else
            return std::pair { *first, *last };

        return std::nullopt;
    }

    // The canonical text's lines in those bars: the header and @length, then
    // each note line in range with the comments above it, a blank line
    // between bars, and the comments after the last line if the range reaches the end.
    std::string barsOfText (const std::string& text, int first, int last, int clipBars)
    {
        std::string out;
        std::vector<std::string> pending; // full-line comments, waiting for the line they belong to
        bool inNotes = false;
        int lastBarShown = 0;

        for (std::size_t start = 0; start < text.size();)
        {
            const auto end = std::min (text.find ('\n', start), text.size());
            const auto line = text.substr (start, end - start);
            start = end + 1;

            if (! inNotes)
            {
                if (! line.empty())
                    out += line + "\n";

                inNotes = line.starts_with ("@length");
                continue;
            }

            if (line.empty())
                continue;

            if (line.starts_with ("#"))
            {
                pending.push_back (line);
                continue;
            }

            int bar = 0;
            std::from_chars (line.data(), line.data() + line.size(), bar);

            if (bar >= first && bar <= last)
            {
                if (bar != lastBarShown)
                    out += "\n";

                for (const auto& comment : pending)
                    out += comment + "\n";

                out += line + "\n";
                lastBarShown = bar;
            }

            pending.clear();
        }

        // Comments after the last note line.
        if (last == clipBars && ! pending.empty())
            for (const auto& comment : pending)
                out += comment + "\n";

        return out;
    }

    ToolResult readClip (const Call& call)
    {
        if (auto error = checkArguments ("read_clip", call.arguments, { { "clip_id", Type::string, true }, { "bars", Type::string, false } });
            ! error.empty())
            return failure (error);

        const auto& project = call.bus.project();
        const auto clipId = call.arguments.at ("clip_id").get<std::string>();
        const project::Track* track = nullptr;
        const auto* clip = project::findClip (project, clipId, &track);

        if (clip == nullptr)
            return failure ("read_clip: " + project::unknownClipMessage (project, clipId, "read_clip"));

        const auto& content = project::notesOf (project, *clip);
        const auto linked = linkedText (project, *clip);
        const auto* drumMap = project::drumMapOf (*track);
        auto text = notes::write (content, project.meter, drumMap);

        std::string part;

        if (const auto bars = optionalString (call.arguments, "bars"))
        {
            std::string error;
            const auto range = parseBars (*bars, content.lengthInBars, error);

            if (! range)
                return failure (error);

            text = barsOfText (text, range->first, range->second, content.lengthInBars);
            part = range->first == range->second ? "bar " + std::to_string (range->first) : "bars " + std::to_string (range->first) + " to "
                                                                                                + std::to_string (range->second);
        }

        const auto details = "clip " + clip->id + " on track " + track->id + " " + doubleQuoted (track->name) + "\n"
                           + "file: " + clip->file + linked + "\n"
                           + "from " + position (project, clip->start) + " to " + position (project, project::clipEnd (project, *clip))
                           + " (@length " + lengthText (content.lengthInBars) + "), " + countText (content.notes.size(), "note") + "\n"
                           + (drumMap != nullptr ? "Its notes are named by " + track->id + "'s drum map, which get_project_overview shows; "
                                                   "notes the map doesn't name are written as note names.\n"
                                                 : std::string())
                           + (part.empty() ? "The next block is the clip's .notes text, exactly as stored. To change the clip, "
                                             "edit that text and send all of it to write_clip, or send the notes that change to patch_clip"
                                           : "The next block is " + part + " of the clip's .notes text, as stored: its header, its @length line, and the "
                                                 "lines whose notes start in " + part + ". patch_clip can change these notes; write_clip needs "
                                                 "the whole text, which read_clip without bars returns")
                           + (linked.empty() ? std::string (".") : std::string ("; its linked clips change with it, since they share the file."));

        return { { details, text }, false };
    }

    // write_clip's and patch_clip's result: the entry, and what the stored text says.
    ToolResult clipWritten (const Call& call, const commands::Result& result, const std::string& clipId, std::string storedNote)
    {
        if (! result.ok)
            return failure (result.message);

        return success (result.message + (storedNote.empty() ? std::string() : "\n" + storedNote) + clipNotices (call.bus, clipId));
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
        const project::Track* track = nullptr;
        const auto& content = project::notesOf (project, *project::findClip (project, clipId, &track));
        const auto stored = notes::write (content, project.meter, project::drumMapOf (*track));
        const auto changes = describeChanges (sent, stored);

        return clipWritten (call, result, clipId,
                            changes.empty() ? "Stored exactly as you sent it."
                                            : "Stored in canonical form, which differs from what you sent: " + changes + ". read_clip returns the stored text.");
    }

    ToolResult patchClip (const Call& call)
    {
        if (auto error = checkArguments ("patch_clip", call.arguments, { { "clip_id", Type::string, true }, { "edits", Type::array, true } });
            ! error.empty())
            return failure (error);

        const auto clipId = call.arguments.at ("clip_id").get<std::string>();
        const auto& editsJson = call.arguments.at ("edits");

        if (editsJson.empty())
            return failure ("patch_clip: 'edits' is empty; give at least one, e.g. [{\"old\": \"1.3 E2 1/8 v90\", \"new\": \"1.3 F2 1/8 v90\"}]");

        std::vector<notes::PatchEdit> edits;

        for (std::size_t i = 0; i < editsJson.size(); ++i)
        {
            const auto label = "patch_clip: edit " + std::to_string (i + 1);

            if (auto error = checkArguments (label, editsJson[i], { { "old", Type::string, false }, { "new", Type::string, false } });
                ! error.empty())
                return failure (error);

            edits.push_back ({ optionalString (editsJson[i], "old"), optionalString (editsJson[i], "new") });
        }

        const auto& project = call.bus.project();
        const project::Track* track = nullptr;
        const auto* clip = project::findClip (project, clipId, &track);

        if (clip == nullptr)
            return failure ("patch_clip: " + project::unknownClipMessage (project, clipId, "patch_clip"));

        const auto* drumMap = project::drumMapOf (*track);
        const auto patched = notes::patch (project::notesOf (project, *clip), edits, project.meter, drumMap);

        if (! patched.clip)
            return failure ("patch_clip on " + clipId + ": " + patched.error);

        const auto result = call.bus.execute (commands::WriteClip { clipId, notes::write (*patched.clip, project.meter, drumMap) }, call.caller);
        auto summary = patched.summary;

        if (! summary.empty())
            summary.front() = static_cast<char> (summary.front() - 'a' + 'A');

        return clipWritten (call, result, clipId, result.ok ? summary + "." : std::string());
    }

    ToolResult addClip (const Call& call)
    {
        if (auto error = checkArguments ("add_clip", call.arguments,
                                         { { "track_id", Type::string, true }, { "start", Type::string, true }, { "bars", Type::integer, false },
                                           { "notes", Type::string, false }, { "linked_to", Type::string, false }, { "copy_of", Type::string, false },
                                           { "id", Type::string, false }, { "file", Type::string, false } });
            ! error.empty())
            return failure (error);

        const auto& arguments = call.arguments;
        const auto given = static_cast<int> (arguments.contains ("bars")) + static_cast<int> (arguments.contains ("notes"))
                         + static_cast<int> (arguments.contains ("linked_to")) + static_cast<int> (arguments.contains ("copy_of"));

        if (given != 1)
            return failure ("add_clip: give exactly one of 'bars' (an empty clip that many bars long), 'notes' (its .notes text), 'linked_to' (a clip "
                            "whose file it shares) or 'copy_of' (a clip whose notes it copies); "
                            + std::string (given == 0 ? "none was given" : std::to_string (given) + " were given"));

        commands::ClipContent content = commands::EmptyClip {};

        if (arguments.contains ("bars"))
        {
            const auto bars = arguments.at ("bars").get<std::int64_t>();

            if (bars < 1 || bars > notes::maxLengthInBars)
                return failure ("add_clip: 'bars' is the empty clip's length, 1 to " + std::to_string (notes::maxLengthInBars) + ", not "
                                + std::to_string (bars));

            content = commands::EmptyClip { static_cast<int> (bars) };
        }
        else if (arguments.contains ("notes"))
            content = commands::ClipText { stringArgument (arguments, "notes") };
        else if (arguments.contains ("linked_to"))
            content = commands::LinkedTo { stringArgument (arguments, "linked_to") };
        else
            content = commands::CopyOf { stringArgument (arguments, "copy_of") };

        const auto& project = call.bus.project();
        const auto before = project::write (project);
        const auto result = call.bus.execute (commands::AddClip { stringArgument (arguments, "track_id"), stringArgument (arguments, "start"),
                                                                  std::move (content), stringArgument (arguments, "id"), stringArgument (arguments, "file") },
                                              call.caller);

        if (! result.ok || ! arguments.contains ("notes"))
            return fromBus (result);

        // The new clip is the one the track didn't have.
        const auto* track = project::findTrack (call.bus.project(), stringArgument (arguments, "track_id"));
        std::string notices;

        if (track != nullptr)
            for (const auto& clip : track->clips)
                if (project::findClip (call.bus.project(), clip.id) != nullptr && result.message.find (" " + clip.id + " ") != std::string::npos)
                    notices = clipNotices (call.bus, clip.id);

        return success (result.message + notices);
    }

    ToolResult moveClip (const Call& call)
    {
        if (auto error = checkArguments ("move_clip", call.arguments,
                                         { { "clip_id", Type::string, true }, { "start", Type::string, true }, { "track_id", Type::string, false } });
            ! error.empty())
            return failure (error);

        return fromBus (call.bus.execute (commands::MoveClip { stringArgument (call.arguments, "clip_id"), stringArgument (call.arguments, "start"),
                                                               stringArgument (call.arguments, "track_id") },
                                          call.caller));
    }

    ToolResult removeClip (const Call& call)
    {
        if (auto error = checkArguments ("remove_clip", call.arguments, { { "clip_id", Type::string, true } }); ! error.empty())
            return failure (error);

        return fromBus (call.bus.execute (commands::RemoveClip { stringArgument (call.arguments, "clip_id") }, call.caller));
    }

    ToolResult makeClipUnique (const Call& call)
    {
        if (auto error = checkArguments ("make_clip_unique", call.arguments, { { "clip_id", Type::string, true }, { "file", Type::string, false } });
            ! error.empty())
            return failure (error);

        return fromBus (call.bus.execute (commands::MakeClipUnique { stringArgument (call.arguments, "clip_id"), stringArgument (call.arguments, "file") },
                                          call.caller));
    }

    //==============================================================================
    ToolResult addTrack (const Call& call)
    {
        if (auto error = checkArguments ("add_track", call.arguments,
                                         { { "type", Type::string, true }, { "name", Type::string, true }, { "instrument", Type::string, false },
                                           { "kit", Type::string, false }, { "before", Type::string, false }, { "after", Type::string, false },
                                           { "id", Type::string, false } });
            ! error.empty())
            return failure (error);

        const auto& arguments = call.arguments;
        std::optional<commands::Instrument> instrument;

        if (const auto type = optionalString (arguments, "instrument"))
            instrument = commands::Instrument { *type, stringArgument (arguments, "kit") };
        else if (arguments.contains ("kit"))
            return failure ("add_track: a kit goes with the instrument \"hocket:drum-sampler\"; give both");

        return fromBus (call.bus.execute (commands::AddTrack { stringArgument (arguments, "type"), stringArgument (arguments, "name"),
                                                               stringArgument (arguments, "id"),
                                                               { stringArgument (arguments, "before"), stringArgument (arguments, "after") },
                                                               std::move (instrument) },
                                          call.caller));
    }

    ToolResult setTrack (const Call& call)
    {
        if (auto error = checkArguments ("set_track", call.arguments,
                                         { { "track_id", Type::string, true }, { "name", Type::string, false }, { "before", Type::string, false },
                                           { "after", Type::string, false }, { "volume", Type::string, false }, { "pan", Type::string, false },
                                           { "mute", Type::boolean, false }, { "solo", Type::boolean, false }, { "instrument", Type::string, false },
                                           { "kit", Type::string, false }, { "drum_map", Type::string, false } });
            ! error.empty())
            return failure (error);

        const auto& arguments = call.arguments;
        const auto trackId = stringArgument (arguments, "track_id");
        std::vector<commands::Command> commands;

        if (const auto name = optionalString (arguments, "name"))
            commands.push_back (commands::RenameTrack { trackId, *name });

        if (arguments.contains ("before") || arguments.contains ("after"))
            commands.push_back (commands::MoveTrack { trackId, { stringArgument (arguments, "before"), stringArgument (arguments, "after") } });

        const auto volume = optionalString (arguments, "volume");
        const auto pan = optionalString (arguments, "pan");
        const auto mute = optionalBool (arguments, "mute");
        const auto solo = optionalBool (arguments, "solo");

        if (volume || pan || mute || solo)
            commands.push_back (commands::SetMix { trackId, volume, pan, mute, solo, {} });

        if (const auto instrument = optionalString (arguments, "instrument"))
        {
            if (*instrument == "none")
            {
                if (arguments.contains ("kit"))
                    return failure ("set_track: a kit goes with the instrument \"hocket:drum-sampler\", not \"none\"");

                commands.push_back (commands::SetInstrument { trackId, std::nullopt });
            }
            else
                commands.push_back (commands::SetInstrument { trackId, commands::Instrument { *instrument, stringArgument (arguments, "kit") } });
        }
        else if (const auto kit = optionalString (arguments, "kit"))
            commands.push_back (commands::SetInstrument { trackId, commands::Instrument { "hocket:drum-sampler", *kit } });

        if (const auto drumMap = optionalString (arguments, "drum_map"))
        {
            std::string error;
            auto changes = parseDrumMapChanges (trackId, *drumMap, error);

            if (! changes)
                return failure (error);

            commands.push_back (std::move (*changes));
        }

        if (commands.empty())
            return failure ("set_track: give at least one of 'name', 'before', 'after', 'volume', 'pan', 'mute', 'solo', 'instrument', 'kit' or "
                            "'drum_map' to change");

        return fromBus (call.bus.executeAsOne (commands, call.caller));
    }

    ToolResult removeTrack (const Call& call)
    {
        if (auto error = checkArguments ("remove_track", call.arguments, { { "track_id", Type::string, true } }); ! error.empty())
            return failure (error);

        return fromBus (call.bus.execute (commands::RemoveTrack { stringArgument (call.arguments, "track_id") }, call.caller));
    }

    ToolResult setSong (const Call& call)
    {
        if (auto error = checkArguments ("set_song", call.arguments, { { "tempo", Type::number, false }, { "time_signature", Type::string, false } });
            ! error.empty())
            return failure (error);

        std::vector<commands::Command> commands;

        if (call.arguments.contains ("tempo"))
            commands.push_back (commands::SetTempo { call.arguments.at ("tempo").get<double>(), {} });

        if (const auto signature = optionalString (call.arguments, "time_signature"))
            commands.push_back (commands::SetTimeSignature { *signature });

        if (commands.empty())
            return failure ("set_song: give 'tempo', 'time_signature', or both");

        return fromBus (call.bus.executeAsOne (commands, call.caller));
    }

    //==============================================================================
    ToolResult transport (const Call& call)
    {
        if (auto error = checkArguments ("transport", call.arguments,
                                         { { "loop_region", Type::string, false }, { "loop", Type::boolean, false }, { "seek", Type::string, false },
                                           { "action", Type::string, false } });
            ! error.empty())
            return failure (error);

        const auto& arguments = call.arguments;

        if (arguments.empty())
        {
            const auto output = outputLine (call.bus);
            return success (transportLine (call.bus) + (output.empty() ? std::string() : "\n" + output));
        }

        std::vector<commands::Command> steps;

        if (const auto region = optionalString (arguments, "loop_region"))
        {
            const auto to = region->find (" to ");

            if (trimmed (*region) == "whole song")
                steps.push_back (commands::ClearLoopRegion {});
            else if (to != std::string::npos)
                steps.push_back (commands::SetLoopRegion { trimmed (region->substr (0, to)), trimmed (region->substr (to + 4)) });
            else
                return failure ("transport: 'loop_region' is a start and an end, \"5.1 to 9.1\", or \"whole song\" to play the whole song, not "
                                + doubleQuoted (*region));
        }

        if (const auto loop = optionalBool (arguments, "loop"))
            steps.push_back (commands::SetLooping { *loop });

        if (const auto seek = optionalString (arguments, "seek"))
            steps.push_back (commands::Seek { *seek });

        if (const auto action = optionalString (arguments, "action"))
        {
            if (*action == "play")
                steps.push_back (commands::Play {});
            else if (*action == "stop")
                steps.push_back (commands::Stop {});
            else
                return failure ("transport: 'action' must be \"play\" or \"stop\", not " + doubleQuoted (*action));
        }

        // In order, each saying what it did; one that fails stops the rest, and says which were done.
        std::vector<std::string> done;

        for (const auto& step : steps)
        {
            const auto result = call.bus.execute (step, call.caller);

            if (! result.ok)
                return failure (result.message + (done.empty() ? std::string() : "\nDone before it: " + joined (done, "; ")));

            done.push_back (result.message);
        }

        return success (joined (done, "\n"));
    }

    //==============================================================================
    ToolResult undo (const Call& call)
    {
        if (auto error = checkArguments ("undo", call.arguments, { { "force", Type::boolean, false } }); ! error.empty())
            return failure (error);

        const auto force = call.arguments.value ("force", false);
        return fromBus (call.bus.undo (call.caller, force ? commands::UndoScope::anyAuthor : commands::UndoScope::callersOwn));
    }

    ToolResult redo (const Call& call)
    {
        if (auto error = checkArguments ("redo", call.arguments, { { "force", Type::boolean, false } }); ! error.empty())
            return failure (error);

        const auto force = call.arguments.value ("force", false);
        return fromBus (call.bus.redo (call.caller, force ? commands::UndoScope::anyAuthor : commands::UndoScope::callersOwn));
    }

    ToolResult history (const Call& call)
    {
        if (auto error = checkArguments ("history", call.arguments, { { "limit", Type::integer, false }, { "author", Type::string, false } });
            ! error.empty())
            return failure (error);

        std::string error;
        const auto limit = listLimit (call.arguments, "history", error);

        if (! limit)
            return failure (error);

        const auto all = call.bus.history();
        const auto author = optionalString (call.arguments, "author");
        std::vector<commands::HistoryEntry> entries;

        const auto matches = [&call, &author] (const commands::HistoryEntry& entry)
        {
            if (! author)
                return true;

            if (*author == "you")
                return entry.author == call.caller;

            if (*author == "others")
                return entry.author != call.caller;

            const auto wanted = lowercase (*author);
            return lowercase (commands::displayName (entry.author)) == wanted || lowercase (commands::nameFor (entry.author, call.caller)) == wanted;
        };

        for (const auto& entry : all)
            if (matches (entry))
                entries.push_back (entry);

        const auto shown = std::min (entries.size(), *limit);
        const auto by = author ? (*author == "you" ? " of yours" : *author == "others" ? " by others" : " by " + *author) : std::string();
        std::string text;

        if (all.empty())
            text = "The history is empty: nothing has changed since Hocket opened the project.";
        else if (entries.empty())
        {
            std::vector<std::string> authors;

            for (const auto& entry : all)
                if (const auto name = commands::nameFor (entry.author, call.caller); std::ranges::find (authors, name) == authors.end())
                    authors.push_back (name);

            const auto whose = *author == "you" ? std::string ("yours") : *author == "others" ? std::string ("by others") : "by " + *author;
            text = (all.size() == 1 ? std::string ("The only entry isn't ") : "None of the " + std::to_string (all.size()) + " entries is ") + whose
                 + ". The history's authors: " + joined (authors, ", ") + ".";
        }
        else if (shown == entries.size())
            text = entries.size() == 1 ? "The only entry" + by + ":" : "Newest first, all " + std::to_string (entries.size()) + " entries" + by + ":";
        else
            text = "Newest first, the latest " + std::to_string (shown) + " of " + std::to_string (entries.size()) + " entries" + by + ":";

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

    //==============================================================================
    ToolResult save (const Call& call)
    {
        if (auto error = checkArguments ("save", call.arguments, { { "name", Type::string, false }, { "overwrite", Type::boolean, false } });
            ! error.empty())
            return failure (error);

        const auto result = call.bus.save ({ stringArgument (call.arguments, "name"), call.arguments.value ("overwrite", false) }, call.caller);
        return { { result.message }, ! result.ok };
    }

    ToolResult listSaves (const Call& call)
    {
        if (auto error = checkArguments ("list_saves", call.arguments, { { "limit", Type::integer, false } }); ! error.empty())
            return failure (error);

        std::string error;
        const auto limit = listLimit (call.arguments, "list_saves", error);

        if (! limit)
            return failure (error);

        const auto saves = call.bus.keptSaves();
        std::string text;

        if (saves.empty())
            text = "No kept saves yet. Each save keeps the version it replaces, so there's one to go back to after the next save.";
        else
        {
            const auto shown = std::min (saves.size(), *limit);
            text = shown == saves.size() ? (saves.size() == 1 ? std::string ("1 kept save:") : std::to_string (saves.size()) + " kept saves, newest first:")
                                         : "The latest " + std::to_string (shown) + " of " + std::to_string (saves.size()) + " kept saves, newest first:";

            for (std::size_t i = 0; i < shown; ++i)
                text += "\n" + commands::describe (saves[saves.size() - 1 - i]);
        }

        text += call.bus.hasUnsavedChanges() ? "\nThe project has unsaved changes." : "\nNothing is unsaved.";
        return success (text);
    }

    ToolResult revert (const Call& call)
    {
        if (auto error = checkArguments ("revert", call.arguments, { { "save", Type::integer, true } }); ! error.empty())
            return failure (error);

        const auto number = call.arguments.at ("save").get<std::int64_t>();

        if (number < 1 || number > 1000000)
            return failure ("revert: 'save' is a kept save's number, as list_saves gives it, e.g. 4 for save #4; not " + std::to_string (number));

        return fromBus (call.bus.execute (commands::Revert { static_cast<int> (number) }, call.caller));
    }

    //==============================================================================
    struct Tool
    {
        std::string_view name;
        bool readOnly;
        std::string_view description; // "{idle}" stands for the idle timeout, "{standard}" for the standard drum map
        std::string_view inputSchema; // JSON
        ToolResult (*run) (const Call&);
    };

    constexpr std::string_view noArguments = R"({ "type": "object", "properties": {}, "additionalProperties": false })";

    constexpr std::array tools {
        Tool { "get_project_overview", true,
               "Summarizes the project open in Hocket: tempo, time signature and song length; the transport and the output level, as "
               "transport with no arguments gives them; every track with its mix, devices, drum map (as changes to the standard one in "
               "write_clip's description, in set_track's drum_map form) and clips (IDs, positions, lengths, note counts, links; a clip's file "
               "only when it isn't the one its ID implies, clp-verse -> clips/verse.notes); the drum kits in use, with the names each plays, "
               "and those installed; the latest history entry, any open transaction, and whether anything is unsaved. Start here: the other "
               "tools take the IDs it lists.",
               noArguments,
               getProjectOverview },

        Tool { "read_clip", true,
               "Returns a clip's notes as .notes text, exactly as Hocket stores them, in a block of its own. The block before it gives the "
               "clip's ID, track, file, position and length. To change a clip, edit that text and send all of it back with write_clip, or "
               "send the notes that change with patch_clip. With bars, only the lines in those bars of the clip are returned, which is "
               "enough for patch_clip but not for write_clip.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string" },
                      "bars": { "type": "string", "description": "The clip's own bars, as its positions count them: \"5-8\", or \"5\"." }
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
               "- position: bar.beat[.sixteenth][+ticks], 1-based and relative to the clip start. A beat is the time "
               "signature's: a quarter note in 4/4, an eighth in 6/8 (get_project_overview gives the signature). 1.1 is "
               "the first beat; 2.3.2 is bar 2, beat 3, second sixteenth of that beat; +ticks adds ticks, 960 per quarter note.\n"
               "- pitch: note name and octave, e.g. C2, F#4, Bb3 (C4 is middle C). A chord is [C3 E3 G3]. On a drums "
               "track, use the names in the track's drum map instead, e.g. \"1.1 [kick hat-closed] 1/16 v100\". Hocket writes "
               "every note the map names by its name, and other notes by note name. The result lists the clip's notes that the "
               "track's kit has no piece for, which are silent. A drums track starts with the standard drum map, and "
               "get_project_overview shows how each track's map differs from it: {standard}.\n"
               "- length: a fraction of a whole note (1/4, 1/8, 3/16), a triplet (1/8t), or ticks (250tk). On a drum "
               "kit, length doesn't change the sound: each hit plays its sample to the end, unless the kit's choke "
               "groups cut it (a closed hat cuts an open one), so write 1/16.\n"
               "- velocity: v1 to v127, required on every line.\n"
               "- # starts a comment, kept as written.\n"
               "Clips linked to this one share its .notes file, so they change too; the result names them. To resize the clip, "
               "change @length; a shorter one must leave out the notes past its new end. A clip, and the whole song, is at most "
               "999 bars long. Not supported yet: controller lines (cc, pb, at for aftertouch), @swing, @ch.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string" },
                      "notes": { "type": "string", "description": "The clip's complete new .notes text." }
                    },
                    "required": [ "clip_id", "notes" ],
                    "additionalProperties": false
                  })",
               writeClip },

        Tool { "patch_clip", false,
               "Changes some of a clip's notes without sending all of them. Each edit has old lines, new lines, or both, in .notes "
               "form: each note in old must be in the clip at that position and pitch, with that length and velocity, and is removed; "
               "each note in new is added. Notes match one by one, whatever the spacing, so one note of a chord or a merged line can be "
               "taken out on its own. A trailing comment in old must match the stored line's; without one, any matches. Edits apply in "
               "order, all or none, and the result is write_clip's. If a note isn't found, or is found with another length, velocity "
               "or comment, the result shows the clip's lines there now. To change @length, use write_clip.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string" },
                      "edits": {
                        "type": "array",
                        "minItems": 1,
                        "items": {
                          "type": "object",
                          "properties": {
                            "old": { "type": "string", "description": "Notes to remove, e.g. \"1.3 E2 1/8 v90\"." },
                            "new": { "type": "string", "description": "Notes to add." }
                          },
                          "additionalProperties": false
                        }
                      }
                    },
                    "required": [ "clip_id", "edits" ],
                    "additionalProperties": false
                  })",
               patchClip },

        Tool { "add_clip", false,
               "Adds a clip to a track at a song position. Give exactly one of: bars, for an empty clip that many bars long; notes, "
               ".notes text as write_clip takes it; linked_to, a clip whose .notes file it shares, so editing either edits both; or "
               "copy_of, a clip whose notes it copies into a file of its own. Clips on a track can't overlap. Hocket makes the new "
               "clip's ID and file name unless you give them; the result names them.",
               R"({
                    "type": "object",
                    "properties": {
                      "track_id": { "type": "string" },
                      "start": { "type": "string", "description": "e.g. \"9.1\"" },
                      "bars": { "type": "integer", "minimum": 1, "maximum": 999 },
                      "notes": { "type": "string" },
                      "linked_to": { "type": "string" },
                      "copy_of": { "type": "string" },
                      "id": { "type": "string", "description": "e.g. \"clp-verse-2\"" },
                      "file": { "type": "string", "description": "e.g. \"clips/verse-2.notes\"; not for a linked clip" }
                    },
                    "required": [ "track_id", "start" ],
                    "additionalProperties": false
                  })",
               addClip },

        Tool { "move_clip", false,
               "Moves a clip to a new start, and to another track if you give one. Its notes keep their places within it. It can't "
               "overlap a clip already there.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string" },
                      "start": { "type": "string", "description": "e.g. \"9.1\"" },
                      "track_id": { "type": "string" }
                    },
                    "required": [ "clip_id", "start" ],
                    "additionalProperties": false
                  })",
               moveClip },

        Tool { "remove_clip", false,
               "Removes a clip. A .notes file no other clip uses leaves the project; the next save moves it to unused/, and undo "
               "brings it back.",
               R"({
                    "type": "object",
                    "properties": { "clip_id": { "type": "string" } },
                    "required": [ "clip_id" ],
                    "additionalProperties": false
                  })",
               removeClip },

        Tool { "make_clip_unique", false,
               "Gives a linked clip a .notes file of its own, with a copy of its notes, so editing it no longer changes the clips "
               "it was linked to. Hocket names the file unless you do.",
               R"({
                    "type": "object",
                    "properties": {
                      "clip_id": { "type": "string" },
                      "file": { "type": "string", "description": "e.g. \"clips/chorus-2.notes\"" }
                    },
                    "required": [ "clip_id" ],
                    "additionalProperties": false
                  })",
               makeClipUnique },

        Tool { "add_track", false,
               "Adds a track: type midi, for notes, or drums, for named drum notes (it gets the standard drum map). Its instrument "
               "is hocket:synth (either type) or hocket:drum-sampler with a kit (drums only; get_project_overview lists the kits), "
               "or none until set_track gives it one. It goes last unless before or after names a track. Hocket makes the track's "
               "ID from its name unless you give one; the result names it and its instrument's.",
               R"({
                    "type": "object",
                    "properties": {
                      "type": { "type": "string", "enum": [ "midi", "drums" ] },
                      "name": { "type": "string", "description": "One line, at most 100 bytes." },
                      "instrument": { "type": "string", "enum": [ "hocket:synth", "hocket:drum-sampler" ] },
                      "kit": { "type": "string", "description": "e.g. \"electronic\"" },
                      "before": { "type": "string" },
                      "after": { "type": "string" },
                      "id": { "type": "string", "description": "e.g. \"trk-bass\"" }
                    },
                    "required": [ "type", "name" ],
                    "additionalProperties": false
                  })",
               addTrack },

        Tool { "set_track", false,
               "Changes any of a track's name, place (before or after another track), mix, instrument, kit and drum map; what one "
               "call changes is one history entry. A track's type can't change: move its clips to a new track instead. A new "
               "kit's names join the drum map; the result says which.",
               R"({
                    "type": "object",
                    "properties": {
                      "track_id": { "type": "string" },
                      "name": { "type": "string" },
                      "before": { "type": "string" },
                      "after": { "type": "string" },
                      "volume": { "type": "string", "description": "\"0dB\" is unity; -96dB to +6dB, or \"-infdB\"" },
                      "pan": { "type": "string", "description": "\"C\", \"L1\" to \"L100\", or \"R1\" to \"R100\"" },
                      "mute": { "type": "boolean" },
                      "solo": { "type": "boolean" },
                      "instrument": { "type": "string", "enum": [ "none", "hocket:synth", "hocket:drum-sampler" ] },
                      "kit": { "type": "string" },
                      "drum_map": { "type": "string", "description": "Changes: \"rim=37\" names note 37 rim, \"-cabasa\" removes a name, e.g. \"rim=37, -cabasa\". Start with \"standard\" or \"empty\" to begin from that map. Notes don't change, only the names clips use for them." }
                    },
                    "required": [ "track_id" ],
                    "additionalProperties": false
                  })",
               setTrack },

        Tool { "remove_track", false,
               "Removes a track and all its clips. Undo brings them back.",
               R"({
                    "type": "object",
                    "properties": { "track_id": { "type": "string" } },
                    "required": [ "track_id" ],
                    "additionalProperties": false
                  })",
               removeTrack },

        Tool { "set_song", false,
               "Sets the tempo, the time signature, or both, as one history entry. A tempo change keeps every note on its beat. A "
               "new time signature keeps every note at the same time, so it's refused unless every clip is a whole number of the "
               "new bars; the result lists the clips it leaves starting off a bar line.",
               R"({
                    "type": "object",
                    "properties": {
                      "tempo": { "type": "number", "minimum": 20, "maximum": 300, "description": "quarter notes per minute" },
                      "time_signature": { "type": "string", "description": "e.g. \"6/8\": the top 1 to 32, the bottom 2, 4, 8 or 16" }
                    },
                    "additionalProperties": false
                  })",
               setSong },

        Tool { "transport", false,
               "Controls playback. Give any of: loop_region, which play plays instead of the whole song; loop, off to play to the "
               "end once and stop; seek, where play starts (while playing, the playhead jumps there); and action, play or stop. "
               "They apply in that order. They last the session but aren't in the history, so each result says what the setting "
               "was before. With no arguments, it says what play plays, where it starts, whether it's playing and where, and the "
               "output's peak level since play last started, how long it has played, and whether it clipped.",
               R"({
                    "type": "object",
                    "properties": {
                      "loop_region": { "type": "string", "description": "\"5.1 to 9.1\", or \"whole song\"" },
                      "loop": { "type": "boolean" },
                      "seek": { "type": "string", "description": "e.g. \"9.1\"" },
                      "action": { "type": "string", "enum": [ "play", "stop" ] }
                    },
                    "additionalProperties": false
                  })",
               transport },

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

        Tool { "redo", false,
               "Puts back the change that undo most recently took back, if nothing has changed since. Like undo, it refuses if "
               "someone else did that undo, unless you pass force, which is for when the user has asked you to.",
               R"({
                    "type": "object",
                    "properties": { "force": { "type": "boolean", "default": false } },
                    "additionalProperties": false
                  })",
               redo },

        Tool { "history", true,
               "Lists the most recent changes, newest first: each entry's number, who made it (you, the user, or another client) "
               "and what it did. author narrows it to one author's entries. Undo always takes back the newest entry, whoever made it.",
               R"({
                    "type": "object",
                    "properties": {
                      "limit": { "type": "integer", "minimum": 1, "maximum": 100, "default": 20 },
                      "author": { "type": "string", "description": "\"you\", \"others\", or a name as history shows it, e.g. \"Lee\"." }
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

        Tool { "save", false,
               "Saves the project to its folder. Save when the user asks you to. Every save keeps the version it replaces as a "
               "kept save, so nothing saved is lost. If a file changed outside Hocket since Hocket last read or wrote it, save "
               "refuses and names it: ask the user, and pass overwrite only if they say to save over it; what was on disk is "
               "kept first.",
               R"({
                    "type": "object",
                    "properties": {
                      "name": { "type": "string", "description": "One line, e.g. \"verse and chorus done\"." },
                      "overwrite": { "type": "boolean", "default": false }
                    },
                    "additionalProperties": false
                  })",
               save },

        Tool { "list_saves", true,
               "Lists the kept saves, newest first: every version of the project that a save replaced, with its number, who saved "
               "it, when, and its name if it has one; and whether the project has unsaved changes. revert brings one back.",
               R"({
                    "type": "object",
                    "properties": { "limit": { "type": "integer", "minimum": 1, "maximum": 100, "default": 20 } },
                    "additionalProperties": false
                  })",
               listSaves },

        Tool { "revert", false,
               "Brings back a kept save as the project, by its number from list_saves, as one history entry that undo takes back. "
               "The files on disk don't change until the next save. Revert only when the user asks: it replaces every change since.",
               R"({
                    "type": "object",
                    "properties": { "save": { "type": "integer", "minimum": 1 } },
                    "required": [ "save" ],
                    "additionalProperties": false
                  })",
               revert },
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

    // "kick2=35, kick=36, ...": the standard drum map, for write_clip's description.
    std::string standardDrumMapText()
    {
        std::string text;

        for (const auto& [note, name] : notes::standardDrumMap().byNote())
            text += (text.empty() ? "" : ", ") + name + "=" + std::to_string (note);

        return text;
    }
}

Json toolDefinitions (std::chrono::seconds idleTimeout)
{
    auto definitions = Json::array();

    for (const auto& tool : tools)
    {
        auto description = replaceAll (std::string (tool.description), "{idle}", durationText (idleTimeout));
        description = replaceAll (std::move (description), "{standard}", standardDrumMapText());

        definitions.push_back ({
            { "name", std::string (tool.name) },
            { "description", std::move (description) },
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
