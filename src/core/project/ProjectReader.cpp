#include "core/Paths.h"
#include "core/project/JsonText.h"
#include "core/project/Project.h"
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
    constexpr std::size_t maxIdLength = 64;
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

    constexpr KeySpec deviceKeys[] = {
        { "id", R"("dev-bass-synth")" },
        { "type", R"("hocket:synth")" },
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

    std::size_t editDistance (std::string_view a, std::string_view b)
    {
        std::vector<std::size_t> row (b.size() + 1);

        for (std::size_t j = 0; j <= b.size(); ++j)
            row[j] = j;

        for (std::size_t i = 1; i <= a.size(); ++i)
        {
            auto diagonal = row[0];
            row[0] = i;

            for (std::size_t j = 1; j <= b.size(); ++j)
            {
                const auto above = row[j];
                row[j] = std::min ({ row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1) });
                diagonal = above;
            }
        }

        return row[b.size()];
    }

    // "bass", "bass-verse-2": lowercase letters and digits, in words joined by single hyphens.
    bool isHyphenatedName (std::string_view name)
    {
        if (name.empty() || name.front() == '-' || name.back() == '-' || name.find ("--") != std::string_view::npos)
            return false;

        return std::ranges::all_of (name, [] (char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
    }

    std::optional<std::string> idProblem (std::string_view id, std::string_view kind, std::string_view prefix, std::string_view example)
    {
        static constexpr std::pair<std::string_view, std::string_view> kinds[] = {
            { "trk-", "track" },
            { "clp-", "clip" },
            { "dev-", "device" },
        };

        const auto kindName = std::string (kind);
        const auto prefixName = std::string (prefix);

        if (! id.starts_with (prefix))
        {
            for (const auto& [otherPrefix, otherKind] : kinds)
                if (id.starts_with (otherPrefix))
                    return singleQuoted (id) + " is a " + std::string (otherKind) + " ID; a " + kindName + " ID starts with "
                         + singleQuoted (prefix) + ", e.g. " + singleQuoted (example);

            return singleQuoted (id) + " isn't a valid " + kindName + " ID: " + kindName + " IDs start with " + singleQuoted (prefix)
                 + ", e.g. " + singleQuoted (example);
        }

        if (id.size() > maxIdLength)
            return singleQuoted (id) + " is too long: IDs are at most " + std::to_string (maxIdLength) + " characters";

        if (! isHyphenatedName (id.substr (prefix.size())))
            return singleQuoted (id) + " isn't a valid " + kindName + " ID: after " + singleQuoted (prefixName)
                 + ", use lowercase letters and digits in words joined by single hyphens, e.g. " + singleQuoted (example);

        return {};
    }

    // Clip files: "clips/<name>.notes" with a hyphenated name. The
    // narrow character set also keeps paths safe and case-insensitive file systems happy.
    bool isValidClipFile (std::string_view file)
    {
        constexpr std::string_view folder = "clips/";
        constexpr std::string_view extension = ".notes";

        return file.starts_with (folder) && file.ends_with (extension) && file.size() > folder.size() + extension.size()
            && isHyphenatedName (file.substr (folder.size(), file.size() - folder.size() - extension.size()));
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
        std::map<std::string, int> idLines;     // ID -> line it was first used on
        std::map<std::string, bool> filesRead; // clip file -> whether it exists and parsed; each is read once

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
        void checkKeys (const Json& object, const std::string& where, std::span<const KeySpec> keys)
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
        std::optional<std::string> readId (const Json& object, std::string_view kind, std::string_view prefix,
                                           std::string_view example, const std::string& where, std::span<const KeySpec> keys)
        {
            const auto id = readString (object, "id", where, keys);

            if (! id)
                return {};

            const auto& value = object.at ("id");

            if (const auto problem = idProblem (*id, kind, prefix, example))
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
                else if (const auto bpm = tempo->get<double>(); bpm < 20.0 || bpm > 999.0)
                    errorAt (*tempo, where + ": \"tempo\" must be between 20 and 999 BPM (quarter notes per minute), e.g. 120");
                else
                    project.tempo = bpm;
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
            const auto id = readId (value, "track", "trk-", "trk-bass", fallback, trackKeys);
            const auto where = id ? "track " + singleQuoted (*id) : fallback;
            checkKeys (value, where, trackKeys);

            if (id)
                track.id = *id;

            if (const auto name = readString (value, "name", where, trackKeys))
                track.name = *name;

            if (const auto type = readString (value, "type", where, trackKeys); type && *type != "midi")
                errorAt (value.at ("type"), where + ": " + jsonQuoted (*type) + " tracks aren't supported yet; so far there are only \"midi\" tracks");

            readMix (value, where, track);

            if (const auto* devices = readArray (value, "devices", where, trackKeys))
            {
                if (devices->size() > 1)
                    errorAt (*devices, where + ": a track has at most one device for now, its instrument (effects come in Milestone 5), e.g. \"devices\": "
                                           + std::string (exampleFor (trackKeys, "devices")));

                for (std::size_t i = 0; i < devices->size(); ++i)
                    readDevice ((*devices)[i], i, where, track);
            }

            std::vector<PlacedClip> placed;

            if (const auto* clips = readArray (value, "clips", where, trackKeys))
                for (std::size_t i = 0; i < clips->size(); ++i)
                    readClip ((*clips)[i], i, where, track, placed);

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

        void readDevice (const Json& value, std::size_t index, const std::string& trackWhere, Track& track)
        {
            const auto fallback = trackWhere + ", devices[" + std::to_string (index) + "]";

            if (! requireObject (value, fallback, deviceKeys))
                return;

            Device device;
            const auto id = readId (value, "device", "dev-", "dev-bass-synth", fallback, deviceKeys);
            const auto where = id ? trackWhere + ", device " + singleQuoted (*id) : fallback;
            checkKeys (value, where, deviceKeys);

            if (id)
                device.id = *id;

            if (const auto type = readString (value, "type", where, deviceKeys); type && *type != "hocket:synth")
                errorAt (value.at ("type"), where + ": " + jsonQuoted (*type)
                                                + (*type == "tracktion:4osc" ? std::string (" is now written \"hocket:synth\": Hocket's synth, which 4OSC plays for now")
                                                                             : std::string (" isn't available; so far there's only \"hocket:synth\", Hocket's synth")));

            track.devices.push_back (std::move (device));
        }

        void readClip (const Json& value, std::size_t index, const std::string& trackWhere, Track& track, std::vector<PlacedClip>& placed)
        {
            const auto fallback = trackWhere + ", clips[" + std::to_string (index) + "]";

            if (! requireObject (value, fallback, clipKeys))
                return;

            Clip clip;
            const auto id = readId (value, "clip", "clp-", "clp-bass-verse", fallback, clipKeys);
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

            if (! file || ! readClipFile (value.at ("file"), *file, where, clip))
                ok = false;

            if (ok)
            {
                const auto end = clipEnd (project, clip);
                const auto songLimit = maxSongLengthInBars * project.meter.ticksPerBar();
                const auto& start = value.at ("start");

                if (end > songLimit)
                {
                    errorAt (start, where + ": it runs from " + notes::formatPosition (clip.start, project.meter) + " to "
                                        + notes::formatPosition (end, project.meter) + ", but a song is at most " + std::to_string (maxSongLengthInBars)
                                        + " bars long, so every clip ends by " + notes::formatPosition (songLimit, project.meter)
                                        + "; start it at " + notes::formatPosition (songLimit - (end - clip.start), project.meter) + " or earlier");
                }
                else
                {
                    placed.push_back ({ clip.id, clip.start, end, offsetOf (start) });
                }
            }

            track.clips.push_back (std::move (clip));
        }

        // Reads the clip's file into project.notesFiles, unless an earlier clip
        // already did: clips that name the same file are linked (SPEC §4.4).
        bool readClipFile (const Json& value, const std::string& file, const std::string& where, Clip& clip)
        {
            if (! isValidClipFile (file))
            {
                errorAt (value, where + ": " + singleQuoted (file) + " isn't a valid clip file: clip files are 'clips/<name>.notes' "
                                        "with a lowercase, hyphenated name, e.g. 'clips/bass-verse.notes'");
                return false;
            }

            clip.file = file;

            // Its errors were reported for the first clip that used it.
            if (const auto read = filesRead.find (file); read != filesRead.end())
                return read->second;

            auto& ok = filesRead[file];
            const auto contents = readFile (file);

            if (! contents)
            {
                errorAt (value, where + ": " + singleQuoted (file) + " doesn't exist in the project folder; create it, "
                                        "or point \"file\" at an existing .notes file");
                return false;
            }

            auto parsed = notes::parse (*contents, project.meter);

            for (auto& error : parsed.errors)
                errors.push_back ({ file, error.line, error.column, std::move (error.message) });

            if (! parsed.clip)
                return false;

            project.notesFiles.emplace (file, std::move (*parsed.clip));
            ok = true;
            return true;
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
