#include "core/Paths.h"
#include "core/Suggestions.h"
#include "core/project/JsonText.h"
#include "core/project/Project.h"
#include "core/project/Rules.h"
#include "core/notes/NotesSyntax.h"
#include "core/units/Units.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <span>
#include <tuple>

namespace hocket::project
{
namespace
{
    using Json = nlohmann::json;

    constexpr std::size_t maxErrors = 20;
    constexpr const char* projectFileName = "project.json";

    struct KeySpec
    {
        std::string_view name;
        std::string_view example; // a valid value, written as JSON
    };

    constexpr KeySpec projectKeys[] = {
        { "format", R"("hocket-project")" },
        { "formatVersion", "0" },
        { "tempo", "120" },
        { "timeSignature", R"("4/4")" },
        { "tracks", "[]" },
    };

    constexpr KeySpec trackKeys[] = {
        { "id", R"("trk-bass")" },
        { "name", R"("Bass")" },
        { "type", R"("midi")" },
        { "volume", R"("0dB")" },
        { "pan", R"("C")" },
        { "mute", "false" },
        { "solo", "false" },
        { "devices", R"([{ "id": "dev-bass-synth", "type": "hocket:synth" }])" },
        { "clips", "[]" },
    };

    constexpr KeySpec drumTrackKeys[] = {
        { "id", R"("trk-drums")" },
        { "name", R"("Drums")" },
        { "type", R"("drums")" },
        { "volume", R"("0dB")" },
        { "pan", R"("C")" },
        { "mute", "false" },
        { "solo", "false" },
        { "devices", R"([{ "id": "dev-drums-kit", "type": "hocket:drum-sampler", "kit": "acoustic" }])" },
        { "drumMap", R"({ "kick": 36, "snare": 38, "hat-closed": 42 })" },
        { "clips", "[]" },
    };

    constexpr KeySpec deviceKeys[] = {
        { "id", R"("dev-bass-synth")" },
        { "type", R"("hocket:synth")" },
    };

    constexpr KeySpec drumSamplerKeys[] = {
        { "id", R"("dev-drums-kit")" },
        { "type", R"("hocket:drum-sampler")" },
        { "kit", R"("acoustic")" },
    };

    // Keys that belong on another kind of object, and what to say about them.
    struct MisplacedKey
    {
        std::string_view name;
        std::string_view message;
    };

    constexpr MisplacedKey misplacedTrackKeys[] = {
        { "drumMap", "only \"drums\" tracks have a \"drumMap\"; other tracks' clips use note names. Make the track \"type\": \"drums\", or remove its drum map" },
    };

    constexpr MisplacedKey misplacedDeviceKeys[] = {
        { "kit", "only a \"hocket:drum-sampler\" has a \"kit\"" },
    };

    constexpr KeySpec clipKeys[] = {
        { "id", R"("clp-bass-verse")" },
        { "file", R"("clips/bass-verse.notes")" },
        { "start", R"("1.1")" },
    };

    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string jsonQuoted (std::string_view text)
    {
        return "\"" + std::string (text) + "\"";
    }

    std::string_view exampleFor (std::span<const KeySpec> keys, std::string_view name)
    {
        for (const auto& key : keys)
            if (key.name == name)
                return key.example;

        return {};
    }

    std::string exampleObject (std::span<const KeySpec> keys)
    {
        std::string text = "{ ";

        for (const auto& key : keys)
            text += (text.size() > 2 ? ", " : "") + jsonQuoted (key.name) + ": " + std::string (key.example);

        return text + " }";
    }

    class Reader
    {
    public:
        Reader (std::string_view projectJson, const FileReader& fileReader)
            : text (projectJson), lines (projectJson), readFile (fileReader)
        {
        }

        LoadResult run()
        {
            // Constructed in place: the value positions must come through intact.
            std::optional<Json> root;

            try
            {
                root.emplace (Json::parse (text));
            }
            catch (const Json::parse_error& e)
            {
                syntaxError (e);
                return finish();
            }

            readProject (*root);
            return finish();
        }

    private:
        // Where a loaded clip is, for the overlap check.
        struct PlacedClip
        {
            std::string id;
            notes::Ticks start = 0;
            notes::Ticks end = 0;
            std::size_t startOffset = 0;
        };

        std::string_view text;
        LineIndex lines;
        const FileReader& readFile;
        Project project;
        std::vector<Diagnostic> errors;
        // A clip file, read once, by the first clip that uses it.
        struct FileRead
        {
            bool ok = false;                          // it exists and parsed
            Track reader;                             // the ID, type and drum map of the track it was read for
            std::set<std::string> otherTracksChecked; // tracks found to name its notes alike, or reported
        };

        std::map<std::string, int> idLines;        // ID -> line it was first used on
        std::map<std::string, FileRead> filesRead; // by clip file
        bool drumMapFailed = false;                // the current track's; its clip files aren't read then

        LoadResult finish()
        {
            // project.json's errors in file order, then the clip files' in load order.
            std::ranges::stable_sort (errors, [] (const Diagnostic& a, const Diagnostic& b)
            {
                const auto rank = [] (const Diagnostic& d)
                {
                    return d.file == projectFileName ? std::tuple (0, d.line, d.column) : std::tuple (1, 0, 0);
                };

                return rank (a) < rank (b);
            });

            if (errors.size() > maxErrors)
            {
                auto stop = errors[maxErrors];
                errors.resize (maxErrors);
                stop.message = "stopping after " + std::to_string (maxErrors) + " errors; fix these first";
                errors.push_back (std::move (stop));
            }

            if (! errors.empty())
                return { {}, std::move (errors) };

            return { std::move (project), {} };
        }

        //==============================================================================
        void errorAtOffset (std::size_t offset, std::string message)
        {
            const auto position = lines.positionOf (offset);
            errors.push_back ({ projectFileName, position.line, position.column, std::move (message) });
        }

        void errorAt (const Json& value, std::string message)
        {
            errorAtOffset (offsetOf (value), std::move (message));
        }

        std::size_t offsetOf (const Json& value) const
        {
            return value.is_string() ? stringStart (text, value.end_pos()) : value.start_pos();
        }

        int lineOf (const Json& value) const
        {
            return lines.positionOf (offsetOf (value)).line;
        }

        void syntaxError (const Json::parse_error& e)
        {
            // e.what() is "[json.exception.parse_error.101] parse error at line 3, column 5: <detail>"
            std::string detail = e.what();

            if (const auto column = detail.find ("column "); column != std::string::npos)
                if (const auto colon = detail.find (": ", column); colon != std::string::npos)
                    detail = detail.substr (colon + 2);

            const auto offset = std::min (e.byte > 0 ? e.byte - 1 : 0, text.size());
            const auto at = offset < text.size() ? text[offset] : '\0';
            auto before = offset;

            while (before > 0 && std::string_view (" \t\r\n").find (text[before - 1]) != std::string_view::npos)
                --before;

            if ((at == '}' || at == ']') && before > 0 && text[before - 1] == ',')
                detail += " (JSON doesn't allow a comma before a closing bracket)";
            else if (at == '/' || (offset > 0 && text[offset - 1] == '/'))
                detail += " (JSON doesn't allow comments)";

            errorAtOffset (offset, "project.json isn't valid JSON: " + detail);
        }

        //==============================================================================
        bool requireObject (const Json& value, const std::string& where, std::span<const KeySpec> keys)
        {
            if (value.is_object())
                return true;

            errorAt (value, where + " should be an object: " + exampleObject (keys));
            return false;
        }

        // Duplicate, unknown and missing keys. Every key is required (SPEC §4.4).
        void checkKeys (const Json& object, const std::string& where, std::span<const KeySpec> keys,
                        std::span<const MisplacedKey> misplaced = {})
        {
            std::set<std::string> seen;

            for (const auto& [key, offset] : keysOfObject (text, object.start_pos()))
            {
                if (! seen.insert (key).second)
                {
                    errorAtOffset (offset, where + ": " + jsonQuoted (key) + " appears twice; keep only one");
                    continue;
                }

                if (! exampleFor (keys, key).empty())
                    continue;

                if (const auto found = std::ranges::find (misplaced, key, &MisplacedKey::name); found != misplaced.end())
                {
                    errorAtOffset (offset, where + ": " + std::string (found->message));
                    continue;
                }

                std::string message = where + ": unknown key " + jsonQuoted (key);

                for (const auto& spec : keys)
                    if (editDistance (key, spec.name) <= 2)
                        message += " (did you mean " + jsonQuoted (spec.name) + "?)";

                message += "; the keys are ";

                for (std::size_t i = 0; i < keys.size(); ++i)
                    message += (i > 0 ? ", " : "") + jsonQuoted (keys[i].name);

                errorAtOffset (offset, std::move (message));
            }

            for (const auto& spec : keys)
                if (! object.contains (spec.name))
                    errorAt (object, where + ": missing " + jsonQuoted (spec.name) + "; add e.g. " + jsonQuoted (spec.name) + ": "
                                         + std::string (spec.example));
        }

        const Json* member (const Json& object, std::string_view key)
        {
            const auto found = object.find (key);
            return found == object.end() ? nullptr : &*found;
        }

        std::optional<std::string> readString (const Json& object, std::string_view key, const std::string& where,
                                               std::span<const KeySpec> keys)
        {
            const auto* value = member (object, key);

            if (value == nullptr)
                return {}; // reported by checkKeys

            if (! value->is_string())
            {
                errorAt (*value, where + ": " + jsonQuoted (key) + " should be a string, e.g. " + std::string (exampleFor (keys, key)));
                return {};
            }

            return value->get<std::string>();
        }

        const Json* readArray (const Json& object, std::string_view key, const std::string& where, std::span<const KeySpec> keys)
        {
            const auto* value = member (object, key);

            if (value == nullptr)
                return nullptr;

            if (! value->is_array())
            {
                errorAt (*value, where + ": " + jsonQuoted (key) + " should be an array, e.g. " + std::string (exampleFor (keys, key)));
                return nullptr;
            }

            return value;
        }

        // Checks the ID's format and that no other object has it. Returns it if it's usable.
        std::optional<std::string> readId (const Json& object, IdKind kind, const std::string& where, std::span<const KeySpec> keys)
        {
            const auto id = readString (object, "id", where, keys);

            if (! id)
                return {};

            const auto& value = object.at ("id");

            if (const auto problem = idProblem (*id, kind))
            {
                errorAt (value, where + ": " + *problem);
                return {};
            }

            const auto [first, inserted] = idLines.try_emplace (*id, lineOf (value));

            if (! inserted)
                errorAt (value, where + ": ID " + singleQuoted (*id) + " is already used on line " + std::to_string (first->second)
                                    + "; IDs must be unique across the project");

            return id;
        }

        //==============================================================================
        void readProject (const Json& root)
        {
            const std::string where = "the project";

            if (! requireObject (root, where, projectKeys))
                return;

            checkKeys (root, where, projectKeys);

            if (const auto format = readString (root, "format", where, projectKeys); format && *format != "hocket-project")
                errorAt (root.at ("format"), where + ": \"format\" must be \"hocket-project\"; this doesn't look like a Hocket project file");

            if (const auto* version = member (root, "formatVersion"))
            {
                if (! version->is_number_integer())
                    errorAt (*version, where + ": \"formatVersion\" should be a whole number, e.g. 0");
                else if (const auto number = version->get<std::int64_t>(); number > formatVersion)
                    errorAt (*version, where + ": the project is format version " + std::to_string (number)
                                           + ", but this build of Hocket reads only format version " + std::to_string (formatVersion)
                                           + "; it was written by a newer Hocket");
                else if (number < formatVersion)
                    errorAt (*version, where + ": \"formatVersion\" must be " + std::to_string (formatVersion));
            }

            if (const auto* tempo = member (root, "tempo"))
            {
                if (! tempo->is_number())
                    errorAt (*tempo, where + ": \"tempo\" should be a number of quarter notes per minute (BPM), e.g. 120");
                else if (const auto problem = tempoProblem (tempo->get<double>()))
                    errorAt (*tempo, where + ": \"tempo\": " + *problem);
                else
                    project.tempo = tempo->get<double>();
            }

            // Read before the tracks: clip starts are positions in it.
            if (const auto signature = readString (root, "timeSignature", where, projectKeys))
            {
                if (const auto meter = notes::parseMeter (*signature); meter.value)
                    project.meter = *meter.value;
                else
                    errorAt (root.at ("timeSignature"), where + ": " + meter.error);
            }

            if (const auto* tracks = readArray (root, "tracks", where, projectKeys))
                for (std::size_t i = 0; i < tracks->size(); ++i)
                    readTrack ((*tracks)[i], i);
        }

        void readTrack (const Json& value, std::size_t index)
        {
            const auto fallback = "tracks[" + std::to_string (index) + "]";

            if (! requireObject (value, fallback, trackKeys))
                return;

            Track track;
            const auto id = readId (value, IdKind::track, fallback, trackKeys);
            const auto where = id ? "track " + singleQuoted (*id) : fallback;

            if (id)
                track.id = *id;

            // The type says which keys the track has.
            if (const auto type = readString (value, "type", where, trackKeys))
            {
                if (const auto parsed = parseTrackType (*type); parsed.value)
                    track.type = *parsed.value;
                else
                    errorAt (value.at ("type"), where + ": " + parsed.error);
            }

            const auto keys = track.type == TrackType::drums ? std::span<const KeySpec> (drumTrackKeys) : std::span<const KeySpec> (trackKeys);
            checkKeys (value, where, keys, misplacedTrackKeys);

            if (const auto name = readString (value, "name", where, keys))
            {
                if (const auto problem = trackNameProblem (*name))
                    errorAt (value.at ("name"), where + ": \"name\": " + *problem);
                else
                    track.name = *name;
            }

            readMix (value, where, track);

            // Before the clips, which are read with it.
            drumMapFailed = false;

            if (track.type == TrackType::drums)
                readDrumMap (value, where, track);

            if (const auto* devices = readArray (value, "devices", where, keys))
            {
                if (devices->size() > 1)
                    errorAt (*devices, where + ": a track has at most one device for now, its instrument (effects come in Milestone 5), e.g. \"devices\": "
                                           + std::string (exampleFor (keys, "devices")));

                for (std::size_t i = 0; i < devices->size(); ++i)
                    readDevice ((*devices)[i], i, where, track);
            }

            std::vector<PlacedClip> placed;

            if (const auto* clips = readArray (value, "clips", where, keys))
            {
                for (std::size_t i = 0; i < clips->size(); ++i)
                    readClip ((*clips)[i], i, where, track, placed);

                // So that a short list of errors isn't taken to mean the clips are fine.
                if (drumMapFailed && ! clips->empty())
                    errorAt (*clips, where + ": " + uncheckedClipsMessage (track, clips->size()));
            }

            checkOverlaps (placed, where);
            project.tracks.push_back (std::move (track));
        }

        // Volume, pan, mute and solo (SPEC §4.4).
        void readMix (const Json& value, const std::string& where, Track& track)
        {
            if (const auto volume = readString (value, "volume", where, trackKeys))
            {
                if (const auto parsed = units::parse (*volume, units::volume()); parsed.value)
                    track.volume = *parsed.value;
                else
                    errorAt (value.at ("volume"), where + ": \"volume\": " + parsed.error);
            }

            if (const auto pan = readString (value, "pan", where, trackKeys))
            {
                if (const auto parsed = units::parsePan (*pan); parsed.value)
                    track.pan = *parsed.value;
                else
                    errorAt (value.at ("pan"), where + ": \"pan\": " + parsed.error);
            }

            if (const auto mute = readBool (value, "mute", where))
                track.mute = *mute;

            if (const auto solo = readBool (value, "solo", where))
                track.solo = *solo;
        }

        std::optional<bool> readBool (const Json& object, std::string_view key, const std::string& where)
        {
            const auto* value = member (object, key);

            if (value == nullptr)
                return {}; // reported by checkKeys

            if (! value->is_boolean())
            {
                errorAt (*value, where + ": " + jsonQuoted (key) + " should be true or false, without quotes");
                return {};
            }

            return value->get<bool>();
        }

        // For a track whose drum map has errors, so its clip files weren't read.
        static std::string uncheckedClipsMessage (const Track& track, std::size_t count)
        {
            std::string ids;

            for (const auto& clip : track.clips)
                if (! clip.id.empty())
                    ids += (ids.empty() ? "" : ", ") + clip.id;

            if (count == 1)
                return "its clip " + (ids.empty() ? std::string() : ids + " ") + "wasn't checked: its .notes file is read with the drum map, "
                                                                                 "so fix the drum map first, and then it will be";

            return "its clips " + (ids.empty() ? std::string() : "(" + ids + ") ") + "weren't checked: their .notes files are read with "
                                                                                      "the drum map, so fix the drum map first, and then they will be";
        }

        // A drums track's names for its notes (SPEC §5.2): { "kick": 36, ... }.
        void readDrumMap (const Json& object, const std::string& where, Track& track)
        {
            const auto* map = member (object, "drumMap");
            const auto example = std::string (exampleFor (drumTrackKeys, "drumMap"));

            if (map == nullptr)
            {
                drumMapFailed = true; // reported by checkKeys
                return;
            }

            if (! map->is_object())
            {
                errorAt (*map, where + ": \"drumMap\" should be an object of drum names and the MIDI notes they play, e.g. " + example);
                drumMapFailed = true;
                return;
            }

            std::vector<DrumMapEntry> entries;
            std::vector<std::pair<std::size_t, const Json*>> places; // each entry's name offset and note value

            for (const auto& [name, offset] : keysOfObject (text, map->start_pos()))
            {
                // Anything that isn't a whole number from 0 to 127 is out of range.
                const auto& note = map->at (name);
                const auto inRange = note.is_number_integer() && note.get<std::int64_t>() >= 0 && note.get<std::int64_t>() <= 127;

                entries.push_back ({ name, inRange ? static_cast<int> (note.get<std::int64_t>()) : -1 });
                places.emplace_back (offset, &note);
            }

            for (auto& problem : buildDrumMap (entries, track.drumMap))
            {
                drumMapFailed = true;
                const auto [nameOffset, note] = places[problem.entry];

                // "kick": "C2"
                if (problem.aboutNote && note->is_string())
                    if (const auto pitch = notes::parsePitch (note->get<std::string>()); pitch.value)
                        problem.message += ", not a note name: drum maps use General MIDI's numbers, and "
                                         + singleQuoted (note->get<std::string>()) + " is " + std::to_string (pitch.value->pitch);

                errorAtOffset (problem.aboutNote ? offsetOf (*note) : nameOffset, where + ": \"drumMap\": " + problem.message);
            }
        }

        void readDevice (const Json& value, std::size_t index, const std::string& trackWhere, Track& track)
        {
            const auto fallback = trackWhere + ", devices[" + std::to_string (index) + "]";

            if (! requireObject (value, fallback, deviceKeys))
                return;

            Device device;
            const auto id = readId (value, IdKind::device, fallback, deviceKeys);
            const auto where = id ? trackWhere + ", device " + singleQuoted (*id) : fallback;

            if (id)
                device.id = *id;

            // The type says which keys the device has.
            if (const auto type = readString (value, "type", where, deviceKeys))
            {
                if (const auto parsed = parseDeviceType (*type); parsed.value)
                    device.type = *parsed.value;
                else
                    errorAt (value.at ("type"), where + ": " + parsed.error);
            }

            const auto keys = device.type == DeviceType::drumSampler ? std::span<const KeySpec> (drumSamplerKeys) : std::span<const KeySpec> (deviceKeys);
            checkKeys (value, where, keys, misplacedDeviceKeys);

            if (const auto problem = devicePlacementProblem (device.type, track.type))
                errorAt (value.at ("type"), where + ": " + *problem + ". Make it \"type\": \"drums\" with a \"drumMap\", or use \"hocket:synth\"");

            if (device.type == DeviceType::drumSampler)
            {
                if (const auto kit = readString (value, "kit", where, keys))
                {
                    if (const auto problem = kitNameProblem (*kit))
                        errorAt (value.at ("kit"), where + ": " + *problem);
                    else
                        device.kit = *kit;
                }
            }

            track.devices.push_back (std::move (device));
        }

        void readClip (const Json& value, std::size_t index, const std::string& trackWhere, Track& track, std::vector<PlacedClip>& placed)
        {
            const auto fallback = trackWhere + ", clips[" + std::to_string (index) + "]";

            if (! requireObject (value, fallback, clipKeys))
                return;

            Clip clip;
            const auto id = readId (value, IdKind::clip, fallback, clipKeys);
            const auto where = id ? trackWhere + ", clip " + singleQuoted (*id) : fallback;
            checkKeys (value, where, clipKeys);

            if (id)
                clip.id = *id;

            bool ok = id.has_value();

            if (const auto start = readString (value, "start", where, clipKeys))
            {
                const auto position = notes::parsePosition (*start, project.meter);

                if (position.value)
                    clip.start = *position.value;
                else
                    errorAt (value.at ("start"), where + ": " + position.error);

                ok = ok && position.value.has_value();
            }
            else
            {
                ok = false;
            }

            const auto file = readString (value, "file", where, clipKeys);

            if (! file || ! readClipFile (value.at ("file"), *file, where, track, clip))
                ok = false;

            if (ok)
            {
                const auto end = clipEnd (project, clip);
                const auto& start = value.at ("start");

                if (const auto problem = songLengthProblem (project.meter, clip.start, end))
                    errorAt (start, where + ": " + *problem);
                else
                    placed.push_back ({ clip.id, clip.start, end, offsetOf (start) });
            }

            track.clips.push_back (std::move (clip));
        }

        // Reads the clip's file into project.notesFiles, with the track's drum
        // map, unless an earlier clip already did: clips that name the same file
        // are linked (SPEC §4.4). A file used on several tracks must mean the
        // same notes on each (SPEC §5.2).
        bool readClipFile (const Json& value, const std::string& file, const std::string& where, const Track& track, Clip& clip)
        {
            if (const auto problem = clipFileProblem (file))
            {
                errorAt (value, where + ": " + *problem);
                return false;
            }

            clip.file = file;

            // Its names would be read wrongly; the drum map's errors say what to fix.
            if (drumMapFailed)
                return false;

            // Its errors were reported for the first clip that used it.
            if (const auto read = filesRead.find (file); read != filesRead.end())
                return read->second.ok && namesAlike (read->second, file, value, where, track);

            auto& read = filesRead[file];
            const auto contents = readFile (file);

            if (! contents)
            {
                errorAt (value, where + ": " + singleQuoted (file) + " doesn't exist in the project folder; create it, "
                                        "or point \"file\" at an existing .notes file");
                return false;
            }

            auto parsed = notes::parse (*contents, project.meter, drumMapOf (track));

            for (auto& error : parsed.errors)
                errors.push_back ({ file, error.line, error.column, std::move (error.message) });

            if (! parsed.clip)
                return false;

            project.notesFiles.emplace (file, std::move (*parsed.clip));
            read.ok = true;
            read.reader.id = track.id;
            read.reader.type = track.type;
            read.reader.drumMap = track.drumMap;
            return true;
        }

        // Whether a track that uses an already read file names its notes as
        // the track it was read for does. Each other track is checked once.
        bool namesAlike (FileRead& read, const std::string& file, const Json& value, const std::string& where, const Track& track)
        {
            if (track.id == read.reader.id || ! read.otherTracksChecked.insert (track.id).second)
                return true;

            const auto conflicts = drumNameConflicts (project.notesFiles.at (file), read.reader, track);

            if (conflicts.empty())
                return true;

            errorAt (value, where + ": " + file + " is also used on track " + singleQuoted (read.reader.id) + ", but the two tracks' "
                                    "drum maps don't name its notes alike: " + conflicts + ". A file used on several tracks must mean the "
                                    "same notes on each, so make the maps agree on these notes, or give this clip a file of its own");
            return false;
        }

        void checkOverlaps (std::vector<PlacedClip>& placed, const std::string& trackWhere)
        {
            std::ranges::stable_sort (placed, {}, &PlacedClip::start);
            const PlacedClip* latest = nullptr; // the clip that ends last so far

            for (const auto& clip : placed)
            {
                if (latest != nullptr && clip.start < latest->end)
                    errorAtOffset (clip.startOffset,
                                   trackWhere + ", clip " + singleQuoted (clip.id) + ": starts at " + notes::formatPosition (clip.start, project.meter)
                                       + ", inside clip " + singleQuoted (latest->id) + " (" + notes::formatPosition (latest->start, project.meter)
                                       + " to " + notes::formatPosition (latest->end, project.meter)
                                       + "); clips on a track can't overlap, so start it at "
                                       + notes::formatPosition (latest->end, project.meter) + " or later");

                if (latest == nullptr || clip.end > latest->end)
                    latest = &clip;
            }
        }
    };
}

LoadResult parse (std::string_view projectJson, const FileReader& readFile)
{
    return Reader (projectJson, readFile).run();
}

LoadResult load (const std::filesystem::path& folder)
{
    std::error_code error;

    if (! std::filesystem::is_directory (folder, error))
        return { {}, { { {}, 0, 0, singleQuoted (utf8FromPath (folder)) + " isn't a folder; a Hocket project is a folder containing project.json" } } };

    const auto projectJson = readTextFile (folder / pathFromUtf8 (projectFileName));

    if (! projectJson)
        return { {}, { { projectFileName, 0, 0, "there's no project.json in " + singleQuoted (utf8FromPath (folder))
                                                     + "; a Hocket project folder needs one (SPEC §4.4)" } } };

    return parse (*projectJson, [&folder] (const std::string& relativePath)
    {
        return readTextFile (folder / pathFromUtf8 (relativePath));
    });
}

std::string formatDiagnostic (const Diagnostic& diagnostic)
{
    if (diagnostic.file.empty())
        return diagnostic.message;

    if (diagnostic.line == 0)
        return diagnostic.file + ": " + diagnostic.message;

    return diagnostic.file + ":" + std::to_string (diagnostic.line) + ":" + std::to_string (diagnostic.column) + ": "
         + diagnostic.message;
}
}
