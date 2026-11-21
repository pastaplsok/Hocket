#include "storage/ProjectFolder.h"

#include "core/Paths.h"
#include "storage/Formats.h"
#include "core/project/Rules.h"
#include "platform/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
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

    //==============================================================================
    // .hocket/save.json: who saved the files beside it, and when.
    //
    //   {
    //     "format": "hocket-save",
    //     "time": "2026-11-20T13:02:37Z",
    //     "author": "Lee",
    //     "name": "before the bridge experiment",
    //     "changedOutside": false,
    //     "discardedRecovery": false
    //   }
    //
    // The time is UTC. An empty author is a version Hocket didn't save: from
    // before Hocket; or, with changedOutside, one of its saves with files
    // changed outside it since; or, with discardedRecovery, a recovery copy
    // that --play --discard-recovery filed here, timed by when it was written.

    std::string saveInfoText (const commands::SaveInfo& info)
    {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["format"] = "hocket-save";
        json["time"] = utcTimeText (info.time);
        json["author"] = info.author;
        json["name"] = info.name;
        json["changedOutside"] = info.changedOutside;
        json["discardedRecovery"] = info.discardedRecovery;
        return json.dump (2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    }

    std::optional<commands::SaveInfo> parseSaveInfo (std::string_view text)
    {
        const auto json = nlohmann::json::parse (text.begin(), text.end(), nullptr, false);

        if (json.is_discarded() || ! json.is_object() || ! json.contains ("time") || ! json.at ("time").is_string())
            return {};

        const auto time = parseUtcTime (json.at ("time").get<std::string>());

        if (! time)
            return {};

        commands::SaveInfo info { *time };

        if (json.contains ("author") && json.at ("author").is_string())
            info.author = json.at ("author").get<std::string>();

        if (json.contains ("name") && json.at ("name").is_string())
            info.name = json.at ("name").get<std::string>();

        if (json.contains ("changedOutside") && json.at ("changedOutside").is_boolean())
            info.changedOutside = json.at ("changedOutside").get<bool>();

        if (json.contains ("discardedRecovery") && json.at ("discardedRecovery").is_boolean())
            info.discardedRecovery = json.at ("discardedRecovery").get<bool>();

        return info;
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

    void removeTemporaries (std::span<const Pending> pending)
    {
        for (const auto& file : pending)
        {
            std::error_code error;
            std::filesystem::remove (file.temporary, error);
        }
    }

    // Puts back the files a failed save already swapped in, newest first.
    // Returns what couldn't be put back, and why.
    std::vector<std::string> putBack (std::span<const Pending> swapped)
    {
        std::vector<std::string> problems;

        for (auto i = swapped.size(); i-- > 0;)
        {
            const auto& file = swapped[i];
            std::string problem;

            if (file.was)
            {
                problem = platform::writeFileToDisk (file.temporary, *file.was);

                if (problem.empty())
                    problem = platform::replaceFile (file.temporary, file.path);

                if (! problem.empty())
                {
                    std::error_code error;
                    std::filesystem::remove (file.temporary, error);
                }
            }
            else
            {
                // It's new: putting the folder back means it isn't there.
                std::error_code error;
                std::filesystem::remove (file.path, error);

                if (error)
                    problem = error.message();
            }

            if (! problem.empty())
                problems.push_back (file.file + " (" + withoutStop (problem) + ")");
        }

        return problems;
    }

    // Where a clip file no clip uses any more goes: "unused/clips/verse-2.notes",
    // or, if that's taken, "unused/clips/verse-2-2.notes", then "-3", and so on.
    std::string unusedPlace (const std::filesystem::path& folder, const std::string& file)
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
            if (! std::filesystem::exists (folder / pathFromUtf8 (place), error) || error)
                return place;
        }
    }

    commands::SaveResult failed (std::string message)
    {
        return { false, "didn't save: " + std::move (message) };
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

std::optional<commands::KeptSave> ProjectFolder::keepVersionOnDisk (const std::vector<std::string>& changedOutside, std::string& problem)
{
    const auto projectPath = folder / pathFromUtf8 (projectFileName);
    std::error_code error;

    if (! std::filesystem::exists (projectPath, error))
        return {}; // a new folder: there's no version to keep

    std::map<std::string, std::string> texts;
    const auto projectText = readTextFile (projectPath);

    if (! projectText)
    {
        problem = "couldn't read project.json";
        return {};
    }

    texts[std::string (projectFileName)] = *projectText;

    // Its clip files: the ones its project.json names, and the ones Hocket
    // knows, should that project.json not read.
    std::set<std::string, std::less<>> files;

    for (const auto& entry : known)
        if (entry.first != projectFileName)
            files.insert (entry.first);

    for (auto& file : clipFilesNamedIn (*projectText))
        files.insert (std::move (file));

    // And any this save overwrites that neither names: one put there by hand.
    for (const auto& file : changedOutside)
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

    // When a file last changed, by its date.
    const auto modifiedTime = [] (const std::filesystem::path& path) -> std::optional<commands::SaveTime>
    {
        std::error_code timeError;
        const auto modified = std::filesystem::last_write_time (path, timeError);

        if (timeError)
            return {};

        return std::chrono::floor<std::chrono::seconds> (std::chrono::clock_cast<std::chrono::system_clock> (modified));
    };

    // Who saved it, and when: its save.json as it is. A version Hocket didn't
    // save is from before Hocket, at project.json's date; one with files
    // changed outside Hocket since its save is labelled so, at the latest change.
    const auto infoText = readTextFile (folder / pathFromUtf8 (saveInfoFile));
    auto info = infoText && changedOutside.empty() ? parseSaveInfo (*infoText) : std::nullopt;

    if (info)
    {
        texts[std::string (saveInfoFile)] = *infoText;
    }
    else
    {
        if (! changedOutside.empty())
        {
            info = commands::SaveInfo { std::chrono::floor<std::chrono::seconds> (clock()), {}, {}, true };
            std::optional<commands::SaveTime> latest;

            for (const auto& file : changedOutside)
                if (const auto time = modifiedTime (folder / pathFromUtf8 (file)); time && (! latest || *time > *latest))
                    latest = time;

            if (latest)
                info->time = *latest;
        }
        else
        {
            const auto time = modifiedTime (projectPath);
            info = commands::SaveInfo { time ? *time : std::chrono::floor<std::chrono::seconds> (clock()) };
        }

        texts[std::string (saveInfoFile)] = saveInfoText (*info);
    }

    return keepFiles (texts, *info, problem);
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
    const auto copying = savesPath / pathFromUtf8 ("saving " + std::to_string (number));
    std::error_code error;
    std::filesystem::remove_all (copying, error);

    // project.json first and save.json last, so a problem writing them is
    // named as it always was.
    std::vector<const std::pair<const std::string, std::string>*> order;

    for (const auto& entry : texts)
        if (entry.first != saveInfoFile)
            order.insert (entry.first == projectFileName ? order.begin() : order.end(), &entry);

    if (const auto found = texts.find (std::string (saveInfoFile)); found != texts.end())
        order.push_back (&*found);

    std::string failure;

    for (const auto* entry : order)
    {
        const auto to = copying / pathFromUtf8 (entry->first);
        std::filesystem::create_directories (to.parent_path(), error);
        const auto written = error ? error.message() : platform::writeFileToDisk (to, entry->second);

        if (! written.empty())
        {
            failure = "couldn't write its copy of " + entry->first + ": " + withoutStop (written);
            break;
        }
    }

    if (failure.empty())
        if (const auto moved = platform::moveFile (copying, savesPath / pathFromUtf8 (name)); ! moved.empty())
            failure = "couldn't name its copy " + singleQuoted (name) + ": " + withoutStop (moved);

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
    texts[std::string (saveInfoFile)] = saveInfoText (info);
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

    for (const auto& file : pending)
    {
        const auto found = known.find (file.file);

        if (found == known.end() ? ! file.stamp.exists : found->second.stamp == file.stamp)
            continue;

        changedOutside.push_back (file.file);
        howTheyChanged.push_back (found == known.end() ? file.file + " is there already, but Hocket never had it in the project"
                                  : ! file.stamp.exists ? file.file + " was deleted outside Hocket"
                                                        : file.file + " changed outside Hocket");
    }

    if (! changedOutside.empty() && ! request.overwrite)
    {
        std::string changes;

        for (std::size_t i = 0; i < howTheyChanged.size() && i < 10; ++i)
            changes += (i == 0 ? "" : "; ") + howTheyChanged[i];

        if (howTheyChanged.size() > 10)
            changes += "; and " + std::to_string (howTheyChanged.size() - 10) + " more";

        return failed ("it would overwrite changes made since Hocket last read or wrote " + std::string (changedOutside.size() == 1 ? "this file" : "these files")
                       + ": " + changes + ". Nothing on disk changed. Someone may have edited "
                       + (changedOutside.size() == 1 ? "it" : "them") + " by hand: ask before saving over "
                       + (changedOutside.size() == 1 ? "it" : "them") + ". Saving with overwrite saves anyway, "
                       + "keeping the version on disk as a kept save first, so it can be got back");
    }

    // Who saved it, and when, swapped in last. Written with any change, and
    // for a name alone: that names the version on disk.
    const auto projectChanged = ! pending.empty();
    const commands::SaveInfo info { std::chrono::floor<std::chrono::seconds> (clock()), commands::displayName (author), request.name };
    const auto infoText = saveInfoText (info);

    if (projectChanged || ! request.name.empty())
    {
        const std::string file (saveInfoFile);
        const auto path = folder / pathFromUtf8 (file);
        std::optional<std::string> was;

        if (const auto problem = onDisk (path, file, was); ! problem.empty())
            return failed (problem);

        pending.push_back ({ file, &infoText, path, temporaryFor (path), std::move (was) });
    }

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

        if (const auto problem = platform::writeFileToDisk (file.temporary, *file.text); ! problem.empty())
        {
            removeTemporaries (std::span (pending).first (i));
            return failed ("couldn't write the new " + file.file + " beside it, as " + file.file + std::string (temporarySuffix) + ": "
                           + withStop (problem) + " Nothing on disk changed");
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
        std::string problem;
        result.kept = keepVersionOnDisk (changedOutside, problem);

        if (! problem.empty())
        {
            removeTemporaries (pending);
            return failed ("couldn't keep the version on disk before replacing it: " + problem + ". Nothing on disk changed");
        }
    }

    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        const auto& file = pending[i];

        if (const auto problem = platform::replaceFile (file.temporary, file.path); ! problem.empty())
        {
            removeTemporaries (std::span (pending).subspan (i));
            const auto notPutBack = putBack (std::span (pending).first (i));
            const auto message = "couldn't replace " + file.file + ": " + withStop (problem);

            if (! notPutBack.empty())
                return failed (message + " Putting back the files already replaced failed for " + listed (notPutBack)
                               + ", so the folder is part new, part old: save again once the problem is fixed");

            // The folder is as it was, so the copy of it goes too.
            if (result.kept)
            {
                std::error_code error;
                std::filesystem::remove_all (folder / pathFromUtf8 (result.kept->folder), error);
            }

            // Put back, a file has what it had, under a new date. One Hocket
            // knew as it was is still known; one changed outside still isn't.
            for (const auto& back : std::span (pending).first (i))
                if (const auto found = known.find (back.file); found != known.end() && std::ranges::find (changedOutside, back.file) == changedOutside.end())
                    found->second.stamp = project::FileStamp::of (back.path);

            return failed (message + (i == 0 ? " Nothing on disk changed" : " Nothing on disk changed: the files already replaced were put back")
                           + ". If another program has the file open, close it there, then save again");
        }

        if (file.file != saveInfoFile)
            result.written.push_back (file.file);
    }

    for (const auto& file : pending)
        if (file.file != saveInfoFile)
            known[file.file] = { file.written, *file.text };

    // Clip files that were in the project and are still here, but that no
    // clip uses now. project.json no longer names them, so moving them can't
    // break it.
    std::vector<std::string> notMoved;
    std::vector<std::string> leaving;

    for (const auto& entry : known)
        if (entry.first != projectFileName && ! files.contains (entry.first))
            leaving.push_back (entry.first);

    for (const auto& file : leaving)
    {
        const auto path = folder / pathFromUtf8 (file);
        std::error_code error;

        if (! std::filesystem::exists (path, error))
        {
            known.erase (file);
            continue;
        }

        const auto place = unusedPlace (folder, file);
        const auto destination = folder / pathFromUtf8 (place);
        std::filesystem::create_directories (destination.parent_path(), error);
        const auto problem = error ? error.message() : platform::moveFile (path, destination);

        if (! problem.empty())
        {
            notMoved.push_back (file + " (" + withoutStop (problem) + ")"); // the next save tries again
            continue;
        }

        known.erase (file);
        result.moved.push_back ({ file, place });
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
}
