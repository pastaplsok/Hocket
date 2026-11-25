#include "storage/ProjectFolder.h"

#include "core/Paths.h"
#include "storage/Formats.h"
#include "core/project/Rules.h"
#include "platform/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <span>
#include <vector>

namespace hocket::storage
{
namespace
{
    constexpr std::string_view projectFileName = "project.json";

    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    // "a", "a and b", "a, b and c"; past `most`, "a, b, c, d, e and 4 more".
    std::string listed (const std::vector<std::string>& items, std::size_t most = 6)
    {
        const auto shown = items.size() <= most ? items.size() : most - 1;
        std::string text;

        for (std::size_t i = 0; i < shown; ++i)
            text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];

        if (shown < items.size())
            text += " and " + std::to_string (items.size() - shown) + " more";

        return text;
    }

    // An OS message as a sentence, ending with a full stop, or without one
    // where it goes in parentheses.
    std::string withStop (std::string text)
    {
        return text.empty() || text.back() == '.' ? text : text + ".";
    }

    std::string withoutStop (std::string text)
    {
        while (! text.empty() && text.back() == '.')
            text.pop_back();

        return text;
    }

    // Said after a problem with a file whose path is longer than Windows
    // allows without its long-path setting: what to do about it. Empty for
    // any other file.
    std::string tooDeepAdvice (const std::filesystem::path& file)
    {
        std::error_code error;
        const auto full = std::filesystem::absolute (file, error);
        const auto length = (error ? file : full).native().size();

        if (platform::longPathsEnabled() || length < platform::shortPathLimit)
            return {};

        return ": at " + std::to_string (length) + " characters, its path is longer than Windows allows unless its long-path setting is on. "
               "Move the project to a folder with a shorter path, or turn on Windows' long-path setting (LongPathsEnabled)";
    }

    //==============================================================================
    // .hocket/save.json: who saved the files beside it, and when, and what
    // those files were, so a version changed by hand since can be told apart.
    //
    //   {
    //     "format": "hocket-save",
    //     "time": "2026-11-20T13:02:37Z",
    //     "author": "Lee",
    //     "name": "before the bridge experiment",
    //     "changedOutside": false,
    //     "discardedRecovery": false,
    //     "files": {
    //       "clips/bass.notes": "3f2a9c81d4e6b7a0",
    //       "project.json": "c4e1b0a29f8d7e61"
    //     }
    //   }
    //
    // The time is UTC. An empty author is a version Hocket didn't save: from
    // before Hocket; or, with changedOutside, one of its saves with files
    // changed outside it since; or, with discardedRecovery, a recovery copy
    // that --play --discard-recovery filed here, timed by when it was written.
    // "files" are the version's files, project.json and the clip files it
    // names, each with its text's fingerprint. One written before Hocket
    // listed them has none, and is taken at its word.

    using Fingerprints = std::map<std::string, std::string>;

    // 16 hex digits of a text's FNV-1a hash: enough to tell a file changed by
    // hand from the one saved.
    std::string fingerprintOf (std::string_view text)
    {
        return std::format ("{:016x}", fnv1a64 (text));
    }

    // The fingerprints of a version's files: all of them but save.json.
    Fingerprints fingerprintsOf (const std::map<std::string, std::string>& texts)
    {
        Fingerprints fingerprints;

        for (const auto& [file, text] : texts)
            if (file != saveInfoFile)
                fingerprints[file] = fingerprintOf (text);

        return fingerprints;
    }

    std::string saveInfoText (const commands::SaveInfo& info, const Fingerprints& files)
    {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["format"] = "hocket-save";
        json["time"] = utcTimeText (info.time);
        json["author"] = info.author;
        json["name"] = info.name;
        json["changedOutside"] = info.changedOutside;
        json["discardedRecovery"] = info.discardedRecovery;
        json["files"] = nlohmann::ordered_json::object();

        for (const auto& [file, fingerprint] : files)
            json["files"][file] = fingerprint;

        return json.dump (2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    }

    struct SaveJson
    {
        commands::SaveInfo info;
        std::optional<Fingerprints> files; // none in one from before they were listed
    };

    std::optional<SaveJson> parseSaveJson (std::string_view text)
    {
        const auto json = nlohmann::json::parse (text.begin(), text.end(), nullptr, false);

        if (json.is_discarded() || ! json.is_object() || ! json.contains ("time") || ! json.at ("time").is_string())
            return {};

        const auto time = parseUtcTime (json.at ("time").get<std::string>());

        if (! time)
            return {};

        SaveJson read { commands::SaveInfo { *time } };

        if (json.contains ("files") && json.at ("files").is_object())
        {
            read.files.emplace();

            for (const auto& [file, fingerprint] : json.at ("files").items())
                if (fingerprint.is_string())
                    (*read.files)[file] = fingerprint.get<std::string>();
        }

        auto& info = read.info;

        if (json.contains ("author") && json.at ("author").is_string())
            info.author = json.at ("author").get<std::string>();

        if (json.contains ("name") && json.at ("name").is_string())
            info.name = json.at ("name").get<std::string>();

        if (json.contains ("changedOutside") && json.at ("changedOutside").is_boolean())
            info.changedOutside = json.at ("changedOutside").get<bool>();

        if (json.contains ("discardedRecovery") && json.at ("discardedRecovery").is_boolean())
            info.discardedRecovery = json.at ("discardedRecovery").get<bool>();

        return read;
    }

    std::optional<commands::SaveInfo> parseSaveInfo (std::string_view text)
    {
        auto read = parseSaveJson (text);
        return read ? std::optional<commands::SaveInfo> (std::move (read->info)) : std::nullopt;
    }

    // The clip files a project.json names, as far as it can be read: it may
    // be one edited by hand, and not valid. Only names a clip file may have
    // ("clips/<name>.notes"), so nothing outside clips/ is ever copied.
    std::vector<std::string> clipFilesNamedIn (std::string_view projectJson)
    {
        std::vector<std::string> files;
        const auto json = nlohmann::json::parse (projectJson.begin(), projectJson.end(), nullptr, false);

        if (json.is_discarded() || ! json.is_object() || ! json.contains ("tracks") || ! json.at ("tracks").is_array())
            return files;

        for (const auto& track : json.at ("tracks"))
        {
            if (! track.is_object() || ! track.contains ("clips") || ! track.at ("clips").is_array())
                continue;

            for (const auto& clip : track.at ("clips"))
                if (clip.is_object() && clip.contains ("file") && clip.at ("file").is_string())
                    if (auto file = clip.at ("file").get<std::string>(); ! project::clipFileProblem (file))
                        files.push_back (std::move (file));
        }

        return files;
    }

    //==============================================================================
    // A file whose text changed, on its way to disk.
    struct Pending
    {
        std::string file;               // relative to the project folder
        const std::string* text = nullptr;
        std::filesystem::path path;
        std::filesystem::path temporary;
        std::optional<std::string> was; // the text it replaces; nullopt for a new file
        project::FileStamp stamp;       // the file's, taken before its text was read
        project::FileStamp written {};  // the new file's, once written beside it: a rename keeps it
    };

    std::filesystem::path temporaryFor (const std::filesystem::path& path)
    {
        auto temporary = path;
        temporary += pathFromUtf8 (temporarySuffix);
        return temporary;
    }

    // A failed save's own temporary files: their texts are the session's,
    // which still has them. (One an earlier save left was set aside first.)
    void removeTemporaries (std::span<const Pending> pending)
    {
        for (const auto& file : pending)
        {
            std::error_code error;
            std::filesystem::remove (file.temporary, error);
        }
    }

    using Kind = ProjectFolder::SaveStep::Kind;
    using Outcome = ProjectFolder::StepOutcome;

    // What a save, or finishing one, says when the step hook stopped it, as a crash would.
    constexpr std::string_view stoppedByHook = "stopped part-way by the step hook";

    // The OS's message, as it were, for a step the hook made fail.
    constexpr std::string_view failedByHook = "the step hook made it fail";

    // Asks the step hook about a step: a function of the save's or finishing's own.
    using Step = std::function<Outcome (Kind, const std::filesystem::path&)>;

    // Puts back the files a failed save already swapped in, newest first.
    struct PutBack
    {
        std::vector<std::pair<std::string, std::string>> problems; // each that couldn't be put back, and why
        bool crashed = false;                                      // the step hook stopped it, as a crash would
    };

    PutBack putBack (std::span<const Pending> swapped, const Step& step)
    {
        PutBack result;

        for (auto i = swapped.size(); i-- > 0;)
        {
            const auto& file = swapped[i];
            const auto outcome = step (Kind::putBack, file.path);

            if (outcome == Outcome::crash)
            {
                result.crashed = true;
                return result;
            }

            std::string problem (outcome == Outcome::fail ? failedByHook : std::string_view());

            if (problem.empty() && file.was)
            {
                problem = platform::writeFileToDisk (file.temporary, *file.was);

                if (problem.empty())
                    problem = platform::replaceFile (file.temporary, file.path);

                // Its text, the old one, is in the kept save.
                if (! problem.empty())
                {
                    std::error_code error;
                    std::filesystem::remove (file.temporary, error);
                }
            }
            else if (problem.empty())
            {
                // It's new: putting the folder back means it isn't there. Its
                // text is the session's, which still has it.
                std::error_code error;
                std::filesystem::remove (file.path, error);

                if (error)
                    problem = error.message();
            }

            if (! problem.empty())
                result.problems.emplace_back (file.file, withoutStop (problem));
        }

        return result;
    }

    // Where a file goes in unused/, keeping its path: "unused/clips/verse-2.notes",
    // or, if that's taken (or reserved for another), "unused/clips/verse-2-2.notes",
    // then "-3", and so on.
    std::string unusedPlace (const std::filesystem::path& folder, const std::string& file, const std::set<std::string, std::less<>>& reserved = {})
    {
        const auto slash = file.find_last_of ('/');
        const auto dot = file.find_last_of ('.');
        const auto hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
        const auto stem = hasExtension ? file.substr (0, dot) : file;
        const auto extension = hasExtension ? file.substr (dot) : std::string();
        const auto prefix = std::string (unusedFolderName) + "/";

        for (int number = 1;; ++number)
        {
            const auto place = number == 1 ? prefix + file : prefix + stem + "-" + std::to_string (number) + extension;
            std::error_code error;

            // If it can't tell, the move fails and says why.
            if (! reserved.contains (place) && (! std::filesystem::exists (folder / pathFromUtf8 (place), error) || error))
                return place;
        }
    }

    // Moves a file or folder that an earlier save left, relative to the
    // folder, to unused/, keeping its path. Never deleted: its text may be
    // nowhere else. Returns what went wrong, or empty; `place` is where it went.
    std::string setAside (const std::filesystem::path& folder, const std::string& relative, std::string& place)
    {
        place = unusedPlace (folder, relative);
        const auto destination = folder / pathFromUtf8 (place);
        std::error_code error;
        std::filesystem::create_directories (destination.parent_path(), error);
        return error ? error.message() : platform::moveFile (folder / pathFromUtf8 (relative), destination);
    }

    commands::SaveResult failed (std::string message)
    {
        return { false, "didn't save: " + std::move (message) };
    }

    //==============================================================================
    // .hocket/saving.json: there while a save puts its files in place, so the
    // next open can finish it (finishInterruptedSave).
    //
    //   {
    //     "format": "hocket-saving",
    //     "author": "Lee",
    //     "time": "2026-11-20T13:02:37Z",
    //     "kept": ".hocket/saves/4 2026-11-20 08-02 Lee",
    //     "files": [
    //       { "file": "clips/bass.notes",
    //         "before": { "exists": true, "modified": 134079..., "size": 52 },
    //         "after": { "exists": true, "modified": 134079..., "size": 48 } },
    //       ...
    //     ],
    //     "unused": [
    //       { "file": "clips/verse-2.notes", "to": "unused/clips/verse-2.notes",
    //         "stamp": { "exists": true, "modified": 134079..., "size": 30 } }
    //     ]
    //   }
    //
    // Each file's new text is beside it ("<file>.hocket-saving") before this
    // is put in place. "kept" is the kept save holding the version the save
    // replaces, or empty when it replaces no file. The files are in the order
    // they go in place; "before" is each one's stamp as the save found it,
    // and "after" its new text's, beside it and then in place (a rename keeps
    // a file's stamp), so finishing can tell what's done, what's still to do,
    // and what changed since. "unused" are the clip files no clip uses any
    // more, where each goes, and its stamp, which the move keeps.
    struct Marker
    {
        struct File
        {
            std::string file;
            project::FileStamp before, after;
        };

        struct Unused
        {
            std::string file, to;
            project::FileStamp stamp;
        };

        std::string author;
        commands::SaveTime time {};
        std::string kept;
        std::vector<File> files;
        std::vector<Unused> unused;
    };

    nlohmann::ordered_json stampJson (const project::FileStamp& stamp)
    {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["exists"] = stamp.exists;
        json["modified"] = static_cast<std::int64_t> (stamp.modified.time_since_epoch().count());
        json["size"] = static_cast<std::uint64_t> (stamp.size);
        return json;
    }

    std::optional<project::FileStamp> parseStamp (const nlohmann::json& json)
    {
        if (! json.is_object() || ! json.contains ("exists") || ! json.at ("exists").is_boolean() || ! json.contains ("modified")
            || ! json.at ("modified").is_number_integer() || ! json.contains ("size") || ! json.at ("size").is_number_unsigned())
            return {};

        project::FileStamp stamp;
        stamp.exists = json.at ("exists").get<bool>();
        stamp.modified = std::filesystem::file_time_type (std::filesystem::file_time_type::duration (json.at ("modified").get<std::int64_t>()));
        stamp.size = json.at ("size").get<std::uint64_t>();
        return stamp;
    }

    std::string markerText (const Marker& marker)
    {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["format"] = "hocket-saving";
        json["author"] = marker.author;
        json["time"] = utcTimeText (marker.time);
        json["kept"] = marker.kept;
        json["files"] = nlohmann::ordered_json::array();

        for (const auto& file : marker.files)
        {
            nlohmann::ordered_json entry = nlohmann::ordered_json::object();
            entry["file"] = file.file;
            entry["before"] = stampJson (file.before);
            entry["after"] = stampJson (file.after);
            json["files"].push_back (std::move (entry));
        }

        json["unused"] = nlohmann::ordered_json::array();

        for (const auto& file : marker.unused)
        {
            nlohmann::ordered_json entry = nlohmann::ordered_json::object();
            entry["file"] = file.file;
            entry["to"] = file.to;
            entry["stamp"] = stampJson (file.stamp);
            json["unused"].push_back (std::move (entry));
        }

        return json.dump (2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    }

    // project.json, .hocket/save.json, or a clip file: the only files a
    // marker may name, so a damaged one can't touch anything else.
    bool isSavedFile (std::string_view file)
    {
        return file == projectFileName || file == saveInfoFile || ! project::clipFileProblem (file);
    }

    // "unused/clips/verse-2.notes": the only places a marker may move a file to.
    bool isUnusedPlace (std::string_view place)
    {
        const auto prefix = std::string (unusedFolderName) + "/";
        return place.starts_with (prefix) && ! project::clipFileProblem (place.substr (prefix.size()));
    }

    // ".hocket/saves/4 2026-11-20 08-02 Lee": a folder in the kept saves, by
    // a name with no path in it.
    bool isKeptSaveFolder (std::string_view folder)
    {
        const auto prefix = std::string (keptSavesFolder) + "/";

        if (! folder.starts_with (prefix))
            return false;

        const auto name = folder.substr (prefix.size());
        return ! name.empty() && name != "." && name != ".." && name.find_first_of ("/\\:") == std::string_view::npos;
    }

    // nullopt for a marker that doesn't parse, or names anything a save
    // wouldn't.
    std::optional<Marker> parseMarker (std::string_view text)
    {
        const auto json = nlohmann::json::parse (text.begin(), text.end(), nullptr, false);

        const auto stringAt = [] (const nlohmann::json& object, const char* key) -> std::optional<std::string>
        {
            if (! object.contains (key) || ! object.at (key).is_string())
                return {};

            return object.at (key).get<std::string>();
        };

        if (json.is_discarded() || ! json.is_object() || stringAt (json, "format") != "hocket-saving" || ! json.contains ("files")
            || ! json.at ("files").is_array() || ! json.contains ("unused") || ! json.at ("unused").is_array())
            return {};

        Marker marker;
        marker.author = stringAt (json, "author").value_or ("");
        marker.kept = stringAt (json, "kept").value_or ("");

        if (! marker.kept.empty() && ! isKeptSaveFolder (marker.kept))
            return {};

        if (const auto time = parseUtcTime (stringAt (json, "time").value_or ("")))
            marker.time = *time;

        for (const auto& entry : json.at ("files"))
        {
            if (! entry.is_object() || ! entry.contains ("before") || ! entry.contains ("after"))
                return {};

            const auto file = stringAt (entry, "file");
            const auto before = parseStamp (entry.at ("before"));
            const auto after = parseStamp (entry.at ("after"));

            if (! file || ! isSavedFile (*file) || ! before || ! after)
                return {};

            marker.files.push_back ({ *file, *before, *after });
        }

        for (const auto& entry : json.at ("unused"))
        {
            if (! entry.is_object() || ! entry.contains ("stamp"))
                return {};

            const auto file = stringAt (entry, "file");
            const auto to = stringAt (entry, "to");
            const auto stamp = parseStamp (entry.at ("stamp"));

            if (! file || project::clipFileProblem (*file) || ! to || ! isUnusedPlace (*to) || ! stamp)
                return {};

            marker.unused.push_back ({ *file, *to, *stamp });
        }

        return marker;
    }

    // What a save stopped before its marker may have left, relative to the
    // folder: the temporary files beside the project's files and the
    // marker's, and a copy it began for a kept save ("saving <n>").
    std::vector<std::string> saveLeftovers (const std::filesystem::path& folder)
    {
        std::vector<std::string> leftovers;
        std::error_code error;

        for (const auto file : { projectFileName, saveInfoFile, savingMarkerFile })
            if (const auto temporary = std::string (file) + std::string (temporarySuffix); std::filesystem::exists (folder / pathFromUtf8 (temporary), error))
                leftovers.push_back (temporary);

        for (std::filesystem::directory_iterator entry (folder / pathFromUtf8 ("clips"), error), end; ! error && entry != end; entry.increment (error))
            if (const auto name = utf8FromPath (entry->path().filename()); name.ends_with (temporarySuffix))
                leftovers.push_back ("clips/" + name);

        for (std::filesystem::directory_iterator entry (folder / pathFromUtf8 (keptSavesFolder), error), end; ! error && entry != end; entry.increment (error))
            if (const auto name = utf8FromPath (entry->path().filename()); name.starts_with ("saving "))
                leftovers.push_back (std::string (keptSavesFolder) + "/" + name);

        return leftovers;
    }
}

//==============================================================================
std::string keptSaveFolderName (int number, const commands::SaveInfo& info)
{
    auto time = commands::localTimeText (info.time);
    std::ranges::replace (time, ':', '-');

    auto text = std::to_string (number) + " " + time + " " + commands::savedBy (info);

    if (! info.name.empty())
        text += " " + info.name;

    return windowsSafeName (text, maxKeptSaveNameLength);
}

ProjectFolder::ProjectFolder (std::filesystem::path projectFolder, const project::Project& loaded, Clock saveClock)
    : folder (std::move (projectFolder)), clock (saveClock ? std::move (saveClock) : Clock ([] { return std::chrono::system_clock::now(); }))
{
    // A new folder's empty project was never on disk.
    if (loaded == project::Project {} && ! std::filesystem::exists (folder / pathFromUtf8 (projectFileName)))
        return;

    for (auto& [file, text] : project::write (loaded))
        known[file] = { project::FileStamp::of (folder / pathFromUtf8 (file)), std::move (text) };
}

std::pair<project::LoadResult, std::optional<ProjectFolder>> ProjectFolder::open (std::filesystem::path folder, Clock clock)
{
    std::map<std::string, project::FileStamp, std::less<>> stamps;
    auto loaded = project::load (folder, [&folder, &stamps] (const std::string& file)
    {
        stamps[file] = project::FileStamp::of (folder / pathFromUtf8 (file));
    });

    if (! loaded.project)
        return { std::move (loaded), std::nullopt };

    ProjectFolder opened (std::move (folder), project::Project {}, std::move (clock));

    for (auto& [file, text] : project::write (*loaded.project))
        opened.known[file] = { stamps[file], std::move (text) };

    return { std::move (loaded), std::move (opened) };
}

ProjectFolder ProjectFolder::asItIs (std::filesystem::path folder, Clock clock)
{
    ProjectFolder opened (std::move (folder), project::Project {}, std::move (clock));
    opened.known.clear(); // the empty project's project.json, if there's one on disk

    std::vector<std::string> files { std::string (projectFileName) };
    std::error_code error;

    for (std::filesystem::directory_iterator entry (opened.folder / pathFromUtf8 ("clips"), error), end; ! error && entry != end; entry.increment (error))
        if (auto name = "clips/" + utf8FromPath (entry->path().filename()); ! project::clipFileProblem (name))
            files.push_back (std::move (name));

    for (const auto& file : files)
    {
        const auto path = opened.folder / pathFromUtf8 (file);
        const auto stamp = project::FileStamp::of (path); // before reading, as open() does

        if (! stamp.exists)
            continue;

        if (auto text = readTextFile (path))
            opened.known[file] = { stamp, std::move (*text) };
    }

    return opened;
}

std::optional<project::FileStamp> ProjectFolder::knownStamp (std::string_view file) const
{
    const auto found = known.find (file);
    return found != known.end() ? std::optional<project::FileStamp> (found->second.stamp) : std::nullopt;
}

std::optional<std::string> ProjectFolder::knownText (std::string_view file) const
{
    const auto found = known.find (file);
    return found != known.end() ? std::optional<std::string> (found->second.text) : std::nullopt;
}

void ProjectFolder::noteRead (const std::string& file, const project::FileStamp& stamp, std::string text)
{
    known[file] = { stamp, std::move (text) };
}

std::vector<commands::KeptSave> ProjectFolder::keptSaves()
{
    return keptSavesIn (folder);
}

std::vector<commands::KeptSave> keptSavesIn (const std::filesystem::path& folder)
{
    std::vector<commands::KeptSave> saves;
    std::error_code error;

    for (std::filesystem::directory_iterator entry (folder / pathFromUtf8 (keptSavesFolder), error), end; ! error && entry != end;
         entry.increment (error))
    {
        if (! entry->is_directory (error))
            continue;

        // "<number> <time> ...": anything else (a copy that a save left
        // part-way, "saving 4") isn't a kept save.
        const auto name = utf8FromPath (entry->path().filename());
        int number = 0;
        const auto [next, problem] = std::from_chars (name.data(), name.data() + name.size(), number);

        if (problem != std::errc() || number <= 0 || next == name.data() + name.size() || *next != ' ')
            continue;

        // Its save.json says who saved it; one Hocket can't read is from before Hocket, at project.json's date.
        const auto infoText = readTextFile (entry->path() / pathFromUtf8 (saveInfoFile));
        auto info = infoText ? parseSaveInfo (*infoText) : std::nullopt;

        if (! info)
        {
            std::error_code timeError;
            const auto modified = std::filesystem::last_write_time (entry->path() / pathFromUtf8 (projectFileName), timeError);
            info = commands::SaveInfo { timeError ? commands::SaveTime {}
                                                  : std::chrono::floor<std::chrono::seconds> (std::chrono::clock_cast<std::chrono::system_clock> (modified)) };
        }

        saves.push_back ({ number, std::move (*info), std::string (keptSavesFolder) + "/" + name });
    }

    std::ranges::sort (saves, {}, &commands::KeptSave::number);
    return saves;
}

project::LoadResult ProjectFolder::loadKeptSave (const commands::KeptSave& save)
{
    return project::load (folder / pathFromUtf8 (save.folder));
}

std::optional<commands::KeptSave> ProjectFolder::keepVersionOnDisk (const std::vector<std::string>& overwrites, const std::vector<std::string>& changedOutside,
                                                                    std::string& problem)
{
    // A save that only adds files (to a new folder, say) replaces nothing.
    if (overwrites.empty())
        return {};

    const auto projectPath = folder / pathFromUtf8 (projectFileName);
    std::error_code error;
    std::map<std::string, std::string> texts;
    std::optional<std::string> projectText;

    // project.json, if it's there: one deleted outside Hocket leaves the
    // clip files still to keep.
    if (std::filesystem::exists (projectPath, error))
    {
        projectText = readTextFile (projectPath);

        if (! projectText)
        {
            problem = "couldn't read project.json";
            return {};
        }

        texts[std::string (projectFileName)] = *projectText;
    }

    // Its clip files: the ones its project.json names, and the ones Hocket
    // knows, should that project.json not read or not be there.
    std::set<std::string, std::less<>> files;

    for (const auto& entry : known)
        if (entry.first != projectFileName)
            files.insert (entry.first);

    if (projectText)
        for (auto& file : clipFilesNamedIn (*projectText))
            files.insert (std::move (file));

    // And any this save overwrites that neither names: one put there by hand.
    for (const auto& list : { &overwrites, &changedOutside })
        for (const auto& file : *list)
            if (file != projectFileName)
                files.insert (file);

    for (const auto& file : files)
    {
        const auto path = folder / pathFromUtf8 (file);

        if (! std::filesystem::exists (path, error))
            continue;

        auto text = readTextFile (path);

        if (! text)
        {
            problem = "couldn't read " + file;
            return {};
        }

        texts[file] = std::move (*text);
    }

    // Every file the save replaces is in the copy, or it writes nothing (SPEC §4.5).
    for (const auto& file : overwrites)
        if (! texts.contains (file))
        {
            problem = "couldn't copy " + file + ", which the save would replace";
            return {};
        }

    // The version's own files: project.json and the clip files it names.
    std::map<std::string, std::string> version;

    if (projectText)
    {
        version[std::string (projectFileName)] = *projectText;

        for (const auto& file : clipFilesNamedIn (*projectText))
            if (const auto found = texts.find (file); found != texts.end())
                version[file] = found->second;
    }

    std::optional<std::string> saveJson;
    const auto info = describeVersionOnDisk (version, changedOutside, saveJson);
    texts[std::string (saveInfoFile)] = saveJson ? *saveJson : saveInfoText (info, fingerprintsOf (version));
    return keepFiles (texts, info, problem);
}

commands::SaveInfo ProjectFolder::describeVersionOnDisk (const std::map<std::string, std::string>& version, const std::vector<std::string>& changedOutside,
                                                         std::optional<std::string>& saveJson) const
{
    // When a file last changed, by its date.
    const auto modifiedTime = [this] (const std::string& file) -> std::optional<commands::SaveTime>
    {
        std::error_code timeError;
        const auto modified = std::filesystem::last_write_time (folder / pathFromUtf8 (file), timeError);

        if (timeError)
            return {};

        return std::chrono::floor<std::chrono::seconds> (std::chrono::clock_cast<std::chrono::system_clock> (modified));
    };

    const auto infoText = readTextFile (folder / pathFromUtf8 (saveInfoFile));
    const auto read = infoText ? parseSaveJson (*infoText) : std::nullopt;

    // Files changed outside Hocket: those this session saw change, or, if it
    // saw none, those that aren't as save.json lists them (changed, added or
    // deleted by hand while Hocket was closed, or checked out).
    auto outside = changedOutside;

    if (outside.empty() && read && read->files)
    {
        for (const auto& [file, text] : version)
            if (const auto listed = read->files->find (file); listed == read->files->end() || listed->second != fingerprintOf (text))
                outside.push_back (file);

        for (const auto& listed : *read->files)
            if (! version.contains (listed.first))
                outside.push_back (listed.first);
    }

    // As save.json says: who saved it, and when.
    if (outside.empty() && read)
    {
        saveJson = infoText;
        return read->info;
    }

    // Changed outside Hocket since a save: at the latest change.
    if (! outside.empty())
    {
        commands::SaveInfo info { std::chrono::floor<std::chrono::seconds> (clock()), {}, {}, true };
        std::optional<commands::SaveTime> latest;

        for (const auto& file : outside)
            if (const auto time = modifiedTime (file); time && (! latest || *time > *latest))
                latest = time;

        if (latest)
            info.time = *latest;

        return info;
    }

    // Never saved by Hocket: from before Hocket, at project.json's date.
    const auto time = modifiedTime (std::string (projectFileName));
    return commands::SaveInfo { time ? *time : std::chrono::floor<std::chrono::seconds> (clock()) };
}

std::optional<commands::KeptSave> ProjectFolder::keepFiles (const std::map<std::string, std::string>& texts, const commands::SaveInfo& info,
                                                             std::string& problem)
{
    const auto saves = keptSaves();
    const auto number = saves.empty() ? 1 : saves.back().number + 1;
    const auto name = keptSaveFolderName (number, info);
    const auto savesPath = folder / pathFromUtf8 (keptSavesFolder);

    // Copied under a name no kept save has, then named, so a save that stops
    // part-way never leaves half a kept save.
    const auto copyingName = std::string (keptSavesFolder) + "/saving " + std::to_string (number);
    const auto copying = folder / pathFromUtf8 (copyingName);
    std::error_code error;

    // One an earlier save left part-made, by a crash: set aside, never
    // deleted (finishInterruptedSave sets them aside first, as a rule).
    if (std::filesystem::exists (copying, error))
    {
        const auto outcome = step (SaveStep::Kind::setAside, copying);

        if (outcome == StepOutcome::crash)
        {
            problem = stoppedByHook;
            return {};
        }

        std::string place;

        if (const auto setAsideProblem = outcome == StepOutcome::fail ? std::string (failedByHook) : setAside (folder, copyingName, place);
            ! setAsideProblem.empty())
        {
            problem = "couldn't set aside " + singleQuoted (copyingName) + ", which an earlier save left part-made, to " + std::string (unusedFolderName)
                    + "/: " + withoutStop (setAsideProblem);
            return {};
        }
    }

    // project.json first and save.json last, so a problem writing them is
    // named as it always was.
    std::vector<const std::pair<const std::string, std::string>*> order;

    for (const auto& entry : texts)
        if (entry.first != saveInfoFile)
            order.insert (entry.first == projectFileName ? order.begin() : order.end(), &entry);

    if (const auto found = texts.find (std::string (saveInfoFile)); found != texts.end())
        order.push_back (&*found);

    // Its own copy, part-made: copies of files that are on disk as they were,
    // and a save.json the session made, so taking it away loses nothing.
    std::string failure;

    for (const auto* entry : order)
    {
        const auto to = copying / pathFromUtf8 (entry->first);
        const auto outcome = step (SaveStep::Kind::keepFile, to);

        if (outcome == StepOutcome::crash)
        {
            problem = stoppedByHook;
            return {};
        }

        std::filesystem::create_directories (to.parent_path(), error);
        const auto written = outcome == StepOutcome::fail ? std::string (failedByHook)
                           : error                        ? error.message()
                                                          : platform::writeFileToDisk (to, entry->second);

        if (! written.empty())
        {
            failure = "couldn't write its copy of " + entry->first + ": " + withoutStop (written) + tooDeepAdvice (to);
            break;
        }
    }

    const auto kept = savesPath / pathFromUtf8 (name);

    if (failure.empty())
    {
        const auto outcome = step (SaveStep::Kind::nameKept, kept);

        if (outcome == StepOutcome::crash)
        {
            problem = stoppedByHook;
            return {};
        }

        if (const auto moved = outcome == StepOutcome::fail ? std::string (failedByHook) : platform::moveFile (copying, kept); ! moved.empty())
            failure = "couldn't name its copy " + singleQuoted (name) + ": " + withoutStop (moved);
    }

    // Read back at its final name, whose longer paths may be past what
    // Windows allows: a kept save that can't be read isn't one, and the save
    // writes nothing.
    if (failure.empty())
    {
        const auto outcome = step (SaveStep::Kind::readBack, kept);

        if (outcome == StepOutcome::crash)
        {
            problem = stoppedByHook;
            return {};
        }

        for (const auto* entry : order)
        {
            const auto file = kept / pathFromUtf8 (entry->first);

            if (outcome == StepOutcome::fail || readTextFile (file) != entry->second)
            {
                failure = "couldn't read its copy of " + entry->first + " back from " + singleQuoted (std::string (keptSavesFolder) + "/" + name)
                        + (outcome == StepOutcome::fail ? ": " + std::string (failedByHook) : tooDeepAdvice (file));

                // Named back first, so it's never listed as a kept save.
                if (! platform::moveFile (kept, copying).empty())
                    std::filesystem::remove_all (kept, error);

                break;
            }
        }
    }

    if (! failure.empty())
    {
        std::filesystem::remove_all (copying, error);
        problem = std::move (failure);
        return {};
    }

    return commands::KeptSave { number, info, std::string (keptSavesFolder) + "/" + name };
}

std::optional<commands::KeptSave> ProjectFolder::keepRecoveryCopy (const std::filesystem::path& copy, commands::SaveTime written, std::string& problem)
{
    // Its files as they are, whether or not they read as a project: all but
    // its own .hocket/, since a recovery copy holds nothing else.
    std::map<std::string, std::string> texts;
    std::error_code error;

    for (std::filesystem::recursive_directory_iterator entry (copy, error), end; ! error && entry != end; entry.increment (error))
    {
        auto relative = utf8FromPath (entry->path().lexically_relative (copy));
        std::ranges::replace (relative, '\\', '/');

        if (relative == ".hocket")
        {
            entry.disable_recursion_pending();
            continue;
        }

        if (std::error_code typeError; ! entry->is_regular_file (typeError))
            continue;

        auto text = readTextFile (entry->path());

        if (! text)
        {
            problem = "couldn't read the recovery copy's " + relative;
            return {};
        }

        texts[relative] = std::move (*text);
    }

    if (error)
    {
        problem = "couldn't look in the recovery copy, " + singleQuoted (utf8FromPath (copy)) + ": " + withoutStop (error.message());
        return {};
    }

    commands::SaveInfo info { written };
    info.discardedRecovery = true;
    texts[std::string (saveInfoFile)] = saveInfoText (info, fingerprintsOf (texts));
    return keepFiles (texts, info, problem);
}

bool ProjectFolder::matchesKnown (const std::map<std::string, std::string>& files) const
{
    return std::ranges::all_of (files, [this] (const auto& file)
    {
        const auto found = known.find (file.first);
        return found != known.end() && found->second.text == file.second;
    });
}

commands::SaveResult ProjectFolder::save (const project::Project& project, const commands::Save& request, const commands::Author& author)
{
    const auto markerPath = folder / pathFromUtf8 (savingMarkerFile);

    // An older save's marker: one a crash interrupted, which --play finishes
    // before a session opens the folder, or says why it can't. A save now
    // could write over what it was putting in place. Only this session's
    // own may be replaced.
    {
        std::error_code error;
        const auto there = std::filesystem::exists (markerPath, error);

        if (! there && ! error)
            ownMarker.reset();
        else if (! ownMarker || project::FileStamp::of (markerPath) != *ownMarker)
            return failed ("a save interrupted earlier isn't finished: " + std::string (savingMarkerFile)
                           + " is there, describing it, and saving now could write over what it was putting in place. Nothing on disk changed. "
                             "Start --play again: it finishes that save before it reads the folder, or says why it can't and where its files are");
    }

    const auto files = project::write (project);

    // In the order they're swapped in: clip files, then project.json, so
    // project.json never names a clip file that isn't there yet.
    std::vector<std::string> order;

    for (const auto& entry : files)
        if (entry.first != projectFileName)
            order.push_back (entry.first);

    order.emplace_back (projectFileName);

    std::vector<Pending> pending;

    // The file's text now; nullopt if it isn't there. A file that's there
    // but can't be read stops the save.
    const auto onDisk = [] (const std::filesystem::path& path, const std::string& file, std::optional<std::string>& text) -> std::string
    {
        std::error_code error;

        if (! std::filesystem::exists (path, error))
            return {};

        text = readTextFile (path);
        return text ? std::string() : "couldn't read " + file + ", which is there, to compare it with the project: another program may be holding it, "
                                                               "or it isn't a file. Nothing on disk changed";
    };

    for (const auto& file : order)
    {
        const auto& text = files.at (file);
        const auto path = folder / pathFromUtf8 (file);
        const auto stamp = project::FileStamp::of (path); // before reading, so a change while it's read shows later
        std::optional<std::string> was;

        if (const auto problem = onDisk (path, file, was); ! problem.empty())
            return failed (problem);

        if (! was || *was != text)
            pending.push_back ({ file, &text, path, temporaryFor (path), std::move (was), stamp });
        else
            known[file] = { stamp, text }; // what the session has, whatever its date says
    }

    // Files this save would overwrite that changed on disk since Hocket last
    // read or wrote them, or that Hocket never had in the project (a new
    // clip's file, named like one put there by hand).
    std::vector<std::string> changedOutside;
    std::vector<std::string> howTheyChanged;

    // Those files as they are now: an overwrite asked for after a refusal
    // saves over them only if they're still as that refusal found them.
    std::string outsideToken;

    for (const auto& file : pending)
    {
        const auto found = known.find (file.file);

        if (found == known.end() ? ! file.stamp.exists : found->second.stamp == file.stamp)
            continue;

        changedOutside.push_back (file.file);
        howTheyChanged.push_back (found == known.end() ? file.file + " is there already, but Hocket never had it in the project"
                                  : ! file.stamp.exists ? file.file + " was deleted outside Hocket"
                                                        : file.file + " changed outside Hocket");

        outsideToken += file.file + " "
                      + (file.stamp.exists ? std::to_string (file.stamp.modified.time_since_epoch().count()) + ":" + std::to_string (file.stamp.size)
                                           : std::string ("gone"))
                      + ";";
    }

    const auto overwrite = request.overwrite && (request.overwriteOnly.empty() || request.overwriteOnly == outsideToken);

    if (! changedOutside.empty() && ! overwrite)
    {
        std::string changes;

        for (std::size_t i = 0; i < howTheyChanged.size() && i < 10; ++i)
            changes += (i == 0 ? "" : "; ") + howTheyChanged[i];

        if (howTheyChanged.size() > 10)
            changes += "; and " + std::to_string (howTheyChanged.size() - 10) + " more";

        auto refused = failed ("it would overwrite changes made since Hocket last read or wrote " + std::string (changedOutside.size() == 1 ? "this file" : "these files")
                               + ": " + changes + ". Nothing on disk changed. Someone may have edited "
                               + (changedOutside.size() == 1 ? "it" : "them") + " by hand: ask before saving over "
                               + (changedOutside.size() == 1 ? "it" : "them") + ". Saving with overwrite saves anyway, "
                               + "keeping the version on disk as a kept save first, so it can be got back");
        refused.outsideChanges = std::move (howTheyChanged);
        refused.outsideToken = std::move (outsideToken);
        return refused;
    }

    // Who saved it, and when, swapped in last. Written with any change, and
    // for a name alone: that names the version on disk, which keeps who
    // saved it and when, or its label if Hocket didn't (Lee, 2026-11-22). The
    // files on disk are the session's then, so they're the version's.
    const auto projectChanged = ! pending.empty();
    const auto now = std::chrono::floor<std::chrono::seconds> (clock());
    auto info = commands::SaveInfo { now, commands::displayName (author), request.name };

    if (! projectChanged && ! request.name.empty())
    {
        std::optional<std::string> unchanged;
        info = describeVersionOnDisk (files, {}, unchanged);
        info.name = request.name;
    }

    const auto infoText = saveInfoText (info, fingerprintsOf (files));

    if (projectChanged || ! request.name.empty())
    {
        const std::string file (saveInfoFile);
        const auto path = folder / pathFromUtf8 (file);
        std::optional<std::string> was;

        const auto stamp = project::FileStamp::of (path);

        if (const auto problem = onDisk (path, file, was); ! problem.empty())
            return failed (problem);

        pending.push_back ({ file, &infoText, path, temporaryFor (path), std::move (was), stamp });
    }

    // What the save says it set aside: files earlier saves left (by a crash)
    // where it writes, whose texts may be nowhere else.
    std::vector<std::string> setAsideSaid;

    // Sets aside a file an earlier save left, relative to the folder: a
    // problem, or empty; stoppedByHook as a crash would stop it.
    const auto setAsideLeftover = [this, &setAsideSaid] (const std::string& relative) -> std::string
    {
        const auto outcome = step (SaveStep::Kind::setAside, folder / pathFromUtf8 (relative));

        if (outcome == StepOutcome::crash)
            return std::string (stoppedByHook);

        std::string place;
        const auto problem = outcome == StepOutcome::fail ? std::string (failedByHook) : setAside (folder, relative, place);

        if (! problem.empty())
            return "couldn't set aside " + relative + ", which an earlier save left, to " + std::string (unusedFolderName) + "/: " + withoutStop (problem);

        setAsideSaid.push_back (relative + " to " + place);
        return {};
    };

    // Every new text beside its file first, so a problem writing any of them
    // stops the save before anything has changed.
    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        auto& file = pending[i];
        std::error_code error;
        std::filesystem::create_directories (file.path.parent_path(), error);

        if (error)
        {
            removeTemporaries (std::span (pending).first (i));
            return failed ("couldn't make the folder for " + file.file + ": " + withStop (error.message()) + " Nothing on disk changed");
        }

        // One an earlier save left is never written over: its text may be
        // nowhere else. (Anything else in the way makes the write fail.)
        if (std::filesystem::is_regular_file (file.temporary, error))
        {
            if (const auto problem = setAsideLeftover (file.file + std::string (temporarySuffix)); ! problem.empty())
            {
                if (problem == stoppedByHook)
                    return failed (problem);

                removeTemporaries (std::span (pending).first (i));
                return failed (problem + ". Nothing on disk changed");
            }
        }

        const auto outcome = step (SaveStep::Kind::writeTemporary, file.temporary);

        if (outcome == StepOutcome::crash)
            return failed (std::string (stoppedByHook));

        if (const auto problem = outcome == StepOutcome::fail ? std::string (failedByHook) : platform::writeFileToDisk (file.temporary, *file.text);
            ! problem.empty())
        {
            removeTemporaries (std::span (pending).first (i)); // a write that fails removes what it began
            return failed ("couldn't write the new " + file.file + " beside it, as " + file.file + std::string (temporarySuffix) + ": "
                           + withoutStop (problem) + tooDeepAdvice (file.temporary) + ". Nothing on disk changed");
        }

        // Swapped in by a rename, the file keeps this stamp: so a change
        // made outside the moment after can't pass for Hocket's own.
        file.written = project::FileStamp::of (file.temporary);
    }

    commands::SaveResult result { true };

    // The version this replaces, kept whole before anything changes, so no
    // save ever loses one (SPEC §4.5).
    if (projectChanged)
    {
        // The files on disk it replaces: those it writes that are there now.
        std::vector<std::string> overwrites;

        for (const auto& file : pending)
            if (file.was && file.file != saveInfoFile)
                overwrites.push_back (file.file);

        std::string problem;
        result.kept = keepVersionOnDisk (overwrites, changedOutside, problem);

        if (problem == stoppedByHook)
            return failed (problem);

        if (! problem.empty())
        {
            removeTemporaries (pending);
            return failed ("couldn't keep the version on disk before replacing it: " + problem + ". Nothing on disk changed");
        }
    }

    // Clip files that were in the project and are still here, but that no
    // clip uses now: moved to unused/ once project.json no longer names them,
    // each to a place planned now, so the marker can say where.
    std::vector<Marker::Unused> leaving;
    std::set<std::string, std::less<>> reserved;

    for (auto entry = known.begin(); entry != known.end();)
    {
        if (entry->first == projectFileName || files.contains (entry->first))
        {
            ++entry;
            continue;
        }

        if (const auto stamp = project::FileStamp::of (folder / pathFromUtf8 (entry->first)); stamp.exists)
        {
            auto to = unusedPlace (folder, entry->first, reserved);
            reserved.insert (to);
            leaving.push_back ({ entry->first, std::move (to), stamp });
            ++entry;
        }
        else
        {
            entry = known.erase (entry); // gone already
        }
    }

    // Nothing to put in place or move: no marker either.
    if (pending.empty() && leaving.empty())
    {
        result.message = request.name.empty() ? "saved: the files on disk already matched the project, so nothing was written"
                                              : "saved: the files on disk already matched the project, so this save only names them \"" + request.name + "\"";
        return result;
    }

    // From here the save is finished even if a crash stops it: every new text
    // is on disk beside its file, the version it replaces is kept, and the
    // marker says what goes where, so the next open puts the rest in place
    // (finishInterruptedSave). It's written beside its place and then put
    // there, so it's whole or not there at all.
    {
        Marker saving { commands::displayName (author), now, result.kept ? result.kept->folder : std::string() };

        for (const auto& file : pending)
            saving.files.push_back ({ file.file, file.stamp, file.written });

        saving.unused = leaving;

        const auto markerTemporary = temporaryFor (markerPath);
        const auto markerTemporaryName = std::string (savingMarkerFile) + std::string (temporarySuffix);
        std::error_code error;
        std::string problem;

        if (std::filesystem::is_regular_file (markerTemporary, error))
            problem = setAsideLeftover (markerTemporaryName);

        if (problem.empty())
        {
            const auto outcome = step (SaveStep::Kind::writeMarker, markerTemporary);

            if (outcome == StepOutcome::crash)
                return failed (std::string (stoppedByHook));

            std::filesystem::create_directories (markerPath.parent_path(), error);
            problem = outcome == StepOutcome::fail ? std::string (failedByHook) : error ? error.message() : platform::writeFileToDisk (markerTemporary, markerText (saving));
        }

        if (problem.empty())
        {
            const auto outcome = step (SaveStep::Kind::nameMarker, markerPath);

            if (outcome == StepOutcome::crash)
                return failed (std::string (stoppedByHook));

            // Over this session's own, if it left one.
            problem = outcome == StepOutcome::fail ? std::string (failedByHook) : platform::replaceFile (markerTemporary, markerPath);
        }

        if (problem == stoppedByHook)
            return failed (problem);

        if (! problem.empty())
        {
            // Nothing has changed: the save's own files go, the texts they
            // hold being the session's, or on disk as they were.
            std::filesystem::remove (markerTemporary, error);
            removeTemporaries (pending);

            if (result.kept)
                std::filesystem::remove_all (folder / pathFromUtf8 (result.kept->folder), error);

            return failed ("couldn't write " + std::string (savingMarkerFile) + ", which says what a save puts in place in case it's interrupted: "
                           + withoutStop (problem) + ". Nothing on disk changed");
        }

        ownMarker = project::FileStamp::of (markerPath);
    }

    const Step stepOf = [this] (Kind kind, const std::filesystem::path& path) { return step (kind, path); };

    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        const auto& file = pending[i];
        const auto outcome = step (SaveStep::Kind::swap, file.path);

        if (outcome == StepOutcome::crash)
            return failed (std::string (stoppedByHook));

        if (const auto problem = outcome == StepOutcome::fail ? std::string (failedByHook) : platform::replaceFile (file.temporary, file.path);
            ! problem.empty())
        {
            // The session still has every text in these.
            removeTemporaries (std::span (pending).subspan (i));
            const auto back = putBack (std::span (pending).first (i), stepOf);

            if (back.crashed)
                return failed (std::string (stoppedByHook));

            const auto message = "couldn't replace " + file.file + ": " + withStop (problem);

            // What each file swapped in holds now, as Hocket knows it. One left
            // in place has Hocket's new text, so a later save mustn't take it
            // for a change made outside. One put back has what it had, under a
            // new date: one Hocket knew as it was is still known, and one
            // changed outside still isn't.
            std::vector<std::string> named;

            for (const auto& swapped : std::span (pending).first (i))
            {
                if (const auto left = std::ranges::find (back.problems, swapped.file, &std::pair<std::string, std::string>::first); left != back.problems.end())
                {
                    named.push_back (swapped.file + " (" + left->second + ")");

                    if (swapped.file != saveInfoFile)
                        known[swapped.file] = { swapped.written, *swapped.text };
                }
                else if (const auto found = known.find (swapped.file); found != known.end() && std::ranges::find (changedOutside, swapped.file) == changedOutside.end())
                {
                    found->second.stamp = project::FileStamp::of (swapped.path);
                }
            }

            // Part new, part old: the marker stays, saying so, so --play stops
            // if the session ends before a save puts it right (and the kept
            // save it names stays); the next save replaces it.
            if (! back.problems.empty())
                return failed (message + " Putting back the files already replaced failed for " + listed (named)
                               + ", so the folder is part new, part old: save again once the problem is fixed");

            // The folder is as it was, so the marker goes, and then the copy
            // of the folder it names.
            const auto removeOutcome = step (SaveStep::Kind::removeMarker, markerPath);

            if (removeOutcome == StepOutcome::crash)
                return failed (std::string (stoppedByHook));

            std::error_code error;

            if (removeOutcome == StepOutcome::fail)
                error = std::make_error_code (std::errc::io_error);
            else
                std::filesystem::remove (markerPath, error);

            std::string markerLeft;

            if (! error)
            {
                ownMarker.reset();

                if (result.kept)
                    std::filesystem::remove_all (folder / pathFromUtf8 (result.kept->folder), error);
            }
            else
            {
                markerLeft = " (" + std::string (savingMarkerFile) + " couldn't be removed, so the kept save it names stays; the next save replaces it)";
            }

            return failed (message + (i == 0 ? " Nothing on disk changed" : " Nothing on disk changed: the files already replaced were put back") + markerLeft
                           + ". If another program has the file open, close it there, then save again");
        }

        if (file.file != saveInfoFile)
            result.written.push_back (file.file);
    }

    for (const auto& file : pending)
        if (file.file != saveInfoFile)
            known[file.file] = { file.written, *file.text };

    // project.json no longer names the clip files leaving, so moving them can't break it.
    std::vector<std::string> notMoved;

    for (const auto& file : leaving)
    {
        const auto path = folder / pathFromUtf8 (file.file);
        const auto outcome = step (SaveStep::Kind::moveUnused, path);

        if (outcome == StepOutcome::crash)
            return failed (std::string (stoppedByHook));

        const auto destination = folder / pathFromUtf8 (file.to);
        std::error_code error;
        std::filesystem::create_directories (destination.parent_path(), error);
        const auto problem = outcome == StepOutcome::fail ? std::string (failedByHook) : error ? error.message() : platform::moveFile (path, destination);

        if (! problem.empty())
        {
            notMoved.push_back (file.file + " (" + withoutStop (problem) + ")"); // the next save tries again
            continue;
        }

        known.erase (file.file);
        result.moved.push_back ({ file.file, file.to });
    }

    // Done: nothing left for the next open to finish.
    std::string markerLeft;

    {
        const auto outcome = step (SaveStep::Kind::removeMarker, markerPath);

        if (outcome == StepOutcome::crash)
            return failed (std::string (stoppedByHook));

        std::error_code error;

        if (outcome == StepOutcome::fail)
            markerLeft = std::string (failedByHook);
        else if (std::filesystem::remove (markerPath, error); error)
            markerLeft = withoutStop (error.message());

        if (markerLeft.empty())
            ownMarker.reset();
    }

    std::vector<std::string> said;

    if (! result.written.empty())
        said.push_back ("wrote " + listed (result.written));

    if (result.kept)
        said.push_back ("the version it replaced is kept as " + commands::describe (*result.kept));

    if (! result.moved.empty())
    {
        std::vector<std::string> moves;

        for (const auto& move : result.moved)
            moves.push_back (move.from + " to " + move.to);

        said.push_back ("moved " + listed (moves) + ", since no clip uses " + (moves.size() == 1 ? "it" : "them") + " any more");
    }

    if (! notMoved.empty())
        said.push_back ("couldn't move " + listed (notMoved) + " to " + std::string (unusedFolderName) + "/, though no clip uses "
                        + (notMoved.size() == 1 ? "it" : "them") + " any more: the next save tries again");

    if (! setAsideSaid.empty())
        said.push_back ("set aside " + listed (setAsideSaid) + ", which an earlier save left when it was interrupted");

    if (! markerLeft.empty())
        said.push_back ("couldn't remove " + std::string (savingMarkerFile) + " (" + markerLeft + "), which only says what the save was putting in place: "
                        + "the next save replaces it, and the next --play removes it");

    if (said.empty())
        said.emplace_back (request.name.empty() ? "the files on disk already matched the project, so nothing was written"
                                                : "the files on disk already matched the project, so this save only names them \"" + request.name + "\"");

    result.message = "saved: ";

    for (std::size_t i = 0; i < said.size(); ++i)
        result.message += (i == 0 ? "" : "; ") + said[i];

    return result;
}

std::string createProject (const std::filesystem::path& folder, const commands::Author& author)
{
    const auto name = singleQuoted (utf8FromPath (folder));
    std::error_code error;
    const auto status = std::filesystem::status (folder, error);

    if (status.type() == std::filesystem::file_type::none)
        return "couldn't look at " + name + ": " + withoutStop (error.message());

    if (status.type() != std::filesystem::file_type::not_found)
    {
        if (! std::filesystem::is_directory (status))
            return name + " is a file; a new project needs a new or empty folder";

        const auto empty = std::filesystem::is_empty (folder, error);

        if (error)
            return "couldn't look in " + name + ": " + withoutStop (error.message());

        if (! empty)
        {
            const auto isProject = std::filesystem::exists (folder / pathFromUtf8 (projectFileName), error);
            return name + " isn't empty" + (isProject ? std::string (": it holds a project already") : std::string())
                 + ". A new project goes only in a new or empty folder, so it never mixes with files already there; name a new folder, "
                   "and it's made for you";
        }
    }
    else
    {
        std::filesystem::create_directories (folder, error);

        if (error)
            return "couldn't make the folder " + name + ": " + withoutStop (error.message());
    }

    const project::Project empty;
    ProjectFolder projectFolder (folder, empty);
    const auto saved = projectFolder.save (empty, {}, author);
    return saved.ok ? std::string() : saved.message;
}

//==============================================================================
namespace
{
    // An interrupted save as finishing finds it, before it changes anything.
    struct FinishPlan
    {
        std::optional<Marker> marker;         // if it reads and parses
        std::string unreadable;               // why not, if it doesn't
        std::vector<std::string> problems;    // why it can't be finished, if it can't
        std::vector<std::size_t> toSwap;      // marker->files still to put in place
        std::vector<std::size_t> toMove;      // marker->unused still to move
        std::vector<std::string> inPlace;     // files the save had put in place
        std::vector<std::string> temporaries; // its new texts still beside their files, as "<file>.hocket-saving"
        bool held = false;                    // a file couldn't be read: another program may be holding it, which passes
    };

    std::filesystem::path markerPathIn (const std::filesystem::path& folder)
    {
        return folder / pathFromUtf8 (savingMarkerFile);
    }

    // Whether the marker is there; when that can't be told, as if it were.
    bool markerThere (const std::filesystem::path& folder)
    {
        std::error_code error;
        const auto there = std::filesystem::exists (markerPathIn (folder), error);
        return there || error;
    }

    FinishPlan planFinish (const std::filesystem::path& folder)
    {
        FinishPlan plan;
        const auto text = readTextFile (markerPathIn (folder));

        if (! text)
        {
            plan.unreadable = "it can't be read (another program may be holding it)";
            plan.held = true;
            return plan;
        }

        plan.marker = parseMarker (*text);

        if (! plan.marker)
        {
            plan.unreadable = "it doesn't read as a save's record (it may have been changed by hand, or damaged)";
            return plan;
        }

        const auto& marker = *plan.marker;

        for (std::size_t i = 0; i < marker.files.size(); ++i)
        {
            const auto& entry = marker.files[i];
            const auto path = folder / pathFromUtf8 (entry.file);
            const auto now = project::FileStamp::of (path);
            const auto beside = project::FileStamp::of (temporaryFor (path));
            const auto temporaryName = entry.file + std::string (temporarySuffix);

            if (! beside.exists)
            {
                if (now == entry.after)
                    plan.inPlace.push_back (entry.file);
                else
                    plan.problems.push_back (entry.file + " isn't as the save would have put it in place, and its new text, " + temporaryName
                                             + ", is gone: it changed after the save stopped, or something took the new text away");

                continue;
            }

            plan.temporaries.push_back (temporaryName);

            if (beside != entry.after)
            {
                plan.problems.push_back (temporaryName + ", the save's new text, isn't as the save wrote it");
            }
            else if (now != entry.before)
            {
                plan.problems.push_back (entry.file
                                         + (! entry.before.exists ? " was put there" : now.exists ? " changed" : " was deleted")
                                         + " after the save stopped");
            }
            else if (entry.before.exists && entry.file != saveInfoFile)
            {
                // Its text is about to be replaced: it must be in the kept save.
                const auto keptText = marker.kept.empty() ? std::nullopt : readTextFile (folder / pathFromUtf8 (marker.kept) / pathFromUtf8 (entry.file));
                const auto nowText = readTextFile (path);

                if (! nowText)
                {
                    plan.problems.push_back ("couldn't read " + entry.file + " to check that the kept save holds it (another program may be holding it)");
                    plan.held = true;
                    continue;
                }

                if (keptText != nowText)
                {
                    plan.problems.push_back ((marker.kept.empty() ? "no kept save holds " : "the kept save '" + marker.kept + "' doesn't hold ") + entry.file
                                             + " as it is, so putting the save's text in its place would lose it");
                    continue;
                }

                plan.toSwap.push_back (i);
            }
            else
            {
                plan.toSwap.push_back (i);
            }
        }

        for (std::size_t i = 0; i < marker.unused.size(); ++i)
        {
            const auto& entry = marker.unused[i];
            const auto now = project::FileStamp::of (folder / pathFromUtf8 (entry.file));

            // Gone: moved already, or by hand. Either way nothing's left to move.
            if (! now.exists)
                continue;

            if (now != entry.stamp)
                plan.problems.push_back (entry.file + ", which the save was moving to " + std::string (unusedFolderName) + "/ since no clip used it, changed after the save stopped");
            else if (project::FileStamp::of (folder / pathFromUtf8 (entry.to)).exists)
                plan.problems.push_back (entry.to + ", where the save was moving " + entry.file + ", is taken");
            else
                plan.toMove.push_back (i);
        }

        return plan;
    }

    // "by Lee, begun 2026-11-20 08:02"
    std::string whoAndWhen (const Marker& marker)
    {
        return "by " + (marker.author.empty() ? std::string ("someone") : marker.author) + ", begun " + commands::localTimeText (marker.time);
    }

    // Where the version before the save, and the save's own, are on disk: for
    // the person, who decides by hand.
    std::string whereTheVersionsAre (const std::filesystem::path& folder, const FinishPlan& plan)
    {
        std::string text;

        if (plan.marker && ! plan.marker->kept.empty())
        {
            text = "The version before the save is kept whole in '" + plan.marker->kept + "'.";
        }
        else if (plan.marker)
        {
            text = "The save replaced no file that was there, so there's no version before it to keep.";
        }
        else
        {
            const auto saves = keptSavesIn (folder);
            text = saves.empty() ? std::string ("There's no kept save.")
                                 : "The version before the save is most likely the newest kept save, " + commands::describe (saves.back()) + ", in '"
                                       + saves.back().folder + "'.";
        }

        auto temporaries = plan.temporaries;

        // Without the marker, every new text a save left beside its file.
        if (! plan.marker)
            for (const auto& leftover : saveLeftovers (folder))
                if (leftover.ends_with (temporarySuffix) && ! leftover.starts_with (savingMarkerFile))
                    temporaries.push_back (leftover);

        if (! temporaries.empty())
            text += " The save's new " + std::string (temporaries.size() == 1 ? "text" : "texts") + " not yet in place "
                  + (temporaries.size() == 1 ? "is" : "are") + " beside " + (temporaries.size() == 1 ? "its file" : "their files") + ": " + listed (temporaries, 10) + ".";

        if (! plan.inPlace.empty())
            text += " It had put " + listed (plan.inPlace, 10) + " in place.";

        if (temporaries.empty() && plan.inPlace.empty())
            text += " None of the save's new texts are beside their files.";

        return text;
    }

    // The marker as the person would find it, to delete by hand: its full path.
    std::filesystem::path markerToDelete (const std::filesystem::path& folder)
    {
        std::error_code error;
        auto marker = std::filesystem::absolute (markerPathIn (folder), error);
        return (error ? markerPathIn (folder) : marker).make_preferred();
    }

    // What the next --play plays once the marker's gone: the files as they
    // are, part the save's if it had put any in place.
    std::string asTheyAre (const FinishPlan& plan)
    {
        auto text = std::string ("the project's files as they are now");

        if (! plan.inPlace.empty())
            text += ", " + listed (plan.inPlace, 10) + " as the save put " + (plan.inPlace.size() == 1 ? "it" : "them");

        return text;
    }

    // A save that can't be finished, and what `consequence` that has.
    std::string blockedMessage (const std::filesystem::path& folder, const FinishPlan& plan, std::string_view consequence)
    {
        const auto save = plan.marker ? "A save " + whoAndWhen (*plan.marker) + "," : std::string ("A save");
        const auto why = plan.marker ? listed (plan.problems, 10) : std::string (savingMarkerFile) + ", its record of what goes where, is there, but " + plan.unreadable;

        return save + " was interrupted part-way, and can't be finished as it meant, " + std::string (consequence) + ": " + why + ".\n"
             + whereTheVersionsAre (folder, plan) + "\n"
             + (plan.held ? "If another program is holding a file, close it there, and the next --play finishes the save. Otherwise, to go on without "
                            "finishing it, "
                          : "To go on without finishing it, ")
             + "take what you want from those files, then delete '" + utf8FromPath (markerToDelete (folder)) + "', which only records the save. "
             + "The next --play then sets the .hocket-saving files aside in " + std::string (unusedFolderName) + "/, keeping their paths, and plays "
             + asTheyAre (plan) + " (or, if they don't load, says so and where the kept saves are, and plays nothing).";
    }
}

FinishedSave finishInterruptedSave (const std::filesystem::path& folder, const ProjectFolder::StepHook& stepHook)
{
    using Result = FinishedSave::Outcome;

    const auto step = [&stepHook] (Kind kind, const std::filesystem::path& path) { return stepHook ? stepHook ({ kind, path }) : Outcome::proceed; };
    const auto stopped = [] { return FinishedSave { Result::failed, std::string (stoppedByHook) }; };
    FinishedSave finished;

    if (markerThere (folder))
    {
        const auto plan = planFinish (folder);

        // All or nothing: nothing on disk changes unless all of it can be done.
        if (! plan.marker || ! plan.problems.empty())
            return { Result::blocked, blockedMessage (folder, plan, "so nothing on disk was changed, and nothing was played") };

        const auto& marker = *plan.marker;
        const auto markerPath = markerPathIn (folder);

        // A step that fails stops it there: what's done stays done, and the
        // marker says what's left, for the next open.
        const auto failedAt = [&marker] (const std::string& what, const std::string& problem)
        {
            return FinishedSave { Result::failed, "A save " + whoAndWhen (marker) + ", was interrupted part-way, and finishing it failed " + what + " ("
                                                      + withoutStop (problem) + "), so nothing was played. What it did stays done, and the next --play "
                                                      + "finishes the rest. If another program has the file open, close it there." };
        };

        std::vector<std::string> written, moved;

        for (const auto i : plan.toSwap)
        {
            const auto& entry = marker.files[i];
            const auto path = folder / pathFromUtf8 (entry.file);
            const auto outcome = step (Kind::swap, path);

            if (outcome == Outcome::crash)
                return stopped();

            if (const auto problem = outcome == Outcome::fail ? std::string (failedByHook) : platform::replaceFile (temporaryFor (path), path); ! problem.empty())
                return failedAt ("putting " + entry.file + " in place", problem);

            if (entry.file != saveInfoFile)
                written.push_back (entry.file);
        }

        for (const auto i : plan.toMove)
        {
            const auto& entry = marker.unused[i];
            const auto path = folder / pathFromUtf8 (entry.file);
            const auto outcome = step (Kind::moveUnused, path);

            if (outcome == Outcome::crash)
                return stopped();

            const auto destination = folder / pathFromUtf8 (entry.to);
            std::error_code error;
            std::filesystem::create_directories (destination.parent_path(), error);

            if (const auto problem = outcome == Outcome::fail ? std::string (failedByHook) : error ? error.message() : platform::moveFile (path, destination);
                ! problem.empty())
                return failedAt ("moving " + entry.file + " to " + entry.to, problem);

            moved.push_back (entry.file + " to " + entry.to);
        }

        {
            const auto outcome = step (Kind::removeMarker, markerPath);

            if (outcome == Outcome::crash)
                return stopped();

            std::error_code error;

            if (outcome == Outcome::fail)
                return failedAt ("removing its record, " + std::string (savingMarkerFile) + ", once everything was in place", std::string (failedByHook));

            if (std::filesystem::remove (markerPath, error); error)
                return failedAt ("removing its record, " + std::string (savingMarkerFile) + ", once everything was in place", error.message());
        }

        std::vector<std::string> said;

        if (! written.empty())
            said.push_back ("wrote " + listed (written));

        if (! moved.empty())
            said.push_back ("moved " + listed (moved) + ", since no clip uses " + (moved.size() == 1 ? "it" : "them") + " any more");

        finished.outcome = Result::finished;
        finished.message = "Finished a save " + whoAndWhen (marker) + ", which was interrupted part-way: ";

        if (said.empty())
            said.push_back ("everything was in place, so only its record, " + std::string (savingMarkerFile) + ", was left to remove");

        for (std::size_t i = 0; i < said.size(); ++i)
            finished.message += (i == 0 ? "" : "; ") + said[i];

        finished.message += ".";
    }

    // What a save stopped before its marker left: it changed nothing else.
    // Its texts may be nowhere else, so they're set aside, never deleted.
    std::vector<std::string> setAsideSaid;

    for (const auto& leftover : saveLeftovers (folder))
    {
        const auto outcome = step (Kind::setAside, folder / pathFromUtf8 (leftover));

        if (outcome == Outcome::crash)
            return stopped();

        std::string place;

        if (const auto problem = outcome == Outcome::fail ? std::string (failedByHook) : setAside (folder, leftover, place); ! problem.empty())
            return { Result::failed, (finished.message.empty() ? std::string() : finished.message + "\n") + "A save was interrupted before it changed anything, "
                                         + "and setting aside what it left failed for " + leftover + " (" + withoutStop (problem)
                                         + "), so nothing was played: the next --play tries again." };

        setAsideSaid.push_back (leftover + " to " + place);
    }

    if (! setAsideSaid.empty())
    {
        finished.message += (finished.message.empty() ? std::string() : std::string ("\n")) + "A save was interrupted before it changed anything. "
                          + "What it left, whose texts may be nowhere else, is set aside: " + listed (setAsideSaid, 10) + ".";

        if (finished.outcome == Result::nothing)
            finished.outcome = Result::setAside;
    }

    return finished;
}

std::string describeInterruptedSave (const std::filesystem::path& folder)
{
    if (markerThere (folder))
    {
        const auto plan = planFinish (folder);

        // "A save ..." as a note: "Note: a save ...".
        if (! plan.marker || ! plan.problems.empty())
            return "Note: a" + blockedMessage (folder, plan, "so --play won't play it until that's sorted out").substr (1);

        return "Note: a save " + whoAndWhen (*plan.marker) + ", was interrupted part-way (" + std::string (savingMarkerFile)
             + " is there), so the folder may hold part of it. --play finishes it before playing.";
    }

    if (const auto leftovers = saveLeftovers (folder); ! leftovers.empty())
        return "Note: a save was interrupted before it changed anything, and left " + listed (leftovers, 10) + ", which --play sets aside in "
             + std::string (unusedFolderName) + "/.";

    return {};
}

bool hasInterruptedSave (const std::filesystem::path& folder)
{
    return markerThere (folder);
}
}
