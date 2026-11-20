#include "storage/ProjectFolder.h"

#include "core/Paths.h"
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

    //==============================================================================
    // .hocket/save.json: who saved the files beside it, and when.
    //
    //   {
    //     "format": "hocket-save",
    //     "time": "2026-11-20T13:02:37Z",
    //     "author": "Lee",
    //     "name": "before the bridge experiment"
    //   }
    //
    // The time is UTC; an empty author is a version Hocket didn't save.

    std::string saveInfoText (const commands::SaveInfo& info)
    {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["format"] = "hocket-save";
        json["time"] = std::format ("{:%Y-%m-%dT%H:%M:%SZ}", info.time);
        json["author"] = info.author;
        json["name"] = info.name;
        return json.dump (2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    }

    // "2026-11-20T13:02:37Z", exactly as saveInfoText writes it.
    std::optional<commands::SaveTime> parseTime (std::string_view text)
    {
        if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z')
            return {};

        const auto number = [text] (std::size_t at, std::size_t length) -> std::optional<int>
        {
            int value = 0;
            const auto* end = text.data() + at + length;
            const auto [next, error] = std::from_chars (text.data() + at, end, value);
            return error == std::errc() && next == end ? std::optional<int> (value) : std::nullopt;
        };

        const auto year = number (0, 4), month = number (5, 2), day = number (8, 2);
        const auto hour = number (11, 2), minute = number (14, 2), second = number (17, 2);

        if (! year || ! month || ! day || ! hour || ! minute || ! second || *hour > 23 || *minute > 59 || *second > 59)
            return {};

        const std::chrono::year_month_day date { std::chrono::year (*year), std::chrono::month (static_cast<unsigned> (*month)),
                                                 std::chrono::day (static_cast<unsigned> (*day)) };

        if (! date.ok())
            return {};

        return std::chrono::sys_days (date) + std::chrono::hours (*hour) + std::chrono::minutes (*minute) + std::chrono::seconds (*second);
    }

    std::optional<commands::SaveInfo> parseSaveInfo (std::string_view text)
    {
        const auto json = nlohmann::json::parse (text.begin(), text.end(), nullptr, false);

        if (json.is_discarded() || ! json.is_object() || ! json.contains ("time") || ! json.at ("time").is_string())
            return {};

        const auto time = parseTime (json.at ("time").get<std::string>());

        if (! time)
            return {};

        commands::SaveInfo info { *time };

        if (json.contains ("author") && json.at ("author").is_string())
            info.author = json.at ("author").get<std::string>();

        if (json.contains ("name") && json.at ("name").is_string())
            info.name = json.at ("name").get<std::string>();

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
    // The code point starting at text[i], moving i past it; nullopt for an
    // invalid sequence, moving i one byte on.
    std::optional<char32_t> nextCodePoint (std::string_view text, std::size_t& i)
    {
        const auto lead = static_cast<unsigned char> (text[i++]);

        if (lead < 0x80)
            return lead;

        const auto extra = lead >= 0xC2 && lead <= 0xDF ? 1 : lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;

        if (extra == 0 || i + static_cast<std::size_t> (extra) > text.size())
            return {};

        char32_t c = lead & (extra == 1 ? 0x1Fu : extra == 2 ? 0x0Fu : 0x07u);

        for (int k = 0; k < extra; ++k)
        {
            const auto next = static_cast<unsigned char> (text[i + static_cast<std::size_t> (k)]);

            if ((next & 0xC0) != 0x80)
                return {};

            c = (c << 6) | (next & 0x3Fu);
        }

        const char32_t smallest = extra == 1 ? 0x80 : extra == 2 ? 0x800 : 0x10000;

        if (c < smallest || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            return {};

        i += static_cast<std::size_t> (extra);
        return c;
    }

    void appendUtf8 (std::string& out, char32_t c)
    {
        if (c < 0x80)
        {
            out += static_cast<char> (c);
        }
        else if (c < 0x800)
        {
            out += static_cast<char> (0xC0 | (c >> 6));
            out += static_cast<char> (0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char> (0xE0 | (c >> 12));
            out += static_cast<char> (0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char> (0xF0 | (c >> 18));
            out += static_cast<char> (0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char> (0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (c & 0x3F));
        }
    }

    // A name Windows takes as it is (keptSaveFolderName says how).
    std::string windowsSafeName (std::string_view text, std::size_t maxUnits)
    {
        constexpr std::string_view refused = "<>:\"/\\|?*";
        std::string name;
        std::size_t units = 0;

        for (std::size_t i = 0; i < text.size();)
        {
            auto c = nextCodePoint (text, i);

            if (! c)
                continue;

            if (*c < 0x20 || (*c >= 0x7F && *c < 0xA0) || (*c < 0x80 && refused.find (static_cast<char> (*c)) != std::string_view::npos))
                c = U' ';

            if (*c == U' ' && (name.empty() || name.back() == ' '))
                continue;

            const std::size_t size = *c >= 0x10000 ? 2 : 1;

            if (units + size > maxUnits)
                break;

            units += size;
            appendUtf8 (name, *c);
        }

        while (! name.empty() && (name.back() == ' ' || name.back() == '.'))
            name.pop_back();

        return name;
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

    auto text = std::to_string (number) + " " + time + " " + (info.author.empty() ? std::string ("before Hocket") : info.author);

    if (! info.name.empty())
        text += " " + info.name;

    return windowsSafeName (text, maxKeptSaveNameLength);
}

ProjectFolder::ProjectFolder (std::filesystem::path projectFolder, const project::Project& loaded, Clock saveClock)
    : folder (std::move (projectFolder)), clock (saveClock ? std::move (saveClock) : Clock ([] { return std::chrono::system_clock::now(); }))
{
    for (const auto& entry : loaded.notesFiles)
        clipFiles.insert (entry.first);
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

std::optional<commands::KeptSave> ProjectFolder::keepVersionOnDisk (std::string& problem)
{
    const auto projectPath = folder / pathFromUtf8 (projectFileName);
    std::error_code error;

    if (! std::filesystem::exists (projectPath, error))
        return {}; // a new folder: there's no version to keep

    const auto projectText = readTextFile (projectPath);

    if (! projectText)
    {
        problem = "couldn't read project.json";
        return {};
    }

    // Its clip files: the ones its project.json names, and the ones Hocket
    // knows, should that project.json not read.
    auto files = clipFiles;

    for (auto& file : clipFilesNamedIn (*projectText))
        files.insert (std::move (file));

    // Who saved it, and when. A version Hocket didn't save is from before
    // Hocket, at project.json's date.
    const auto infoText = readTextFile (folder / pathFromUtf8 (saveInfoFile));
    auto info = infoText ? parseSaveInfo (*infoText) : std::nullopt;
    const auto infoTextKept = info ? *infoText : std::string();

    if (! info)
    {
        const auto modified = std::filesystem::last_write_time (projectPath, error);
        info = commands::SaveInfo { error ? std::chrono::floor<std::chrono::seconds> (clock())
                                          : std::chrono::floor<std::chrono::seconds> (std::chrono::clock_cast<std::chrono::system_clock> (modified)) };
    }

    const auto saves = keptSaves();
    const auto number = saves.empty() ? 1 : saves.back().number + 1;
    const auto name = keptSaveFolderName (number, *info);
    const auto savesPath = folder / pathFromUtf8 (keptSavesFolder);

    // Copied under a name no kept save has, then named, so a save that stops
    // part-way never leaves half a kept save.
    const auto copying = savesPath / pathFromUtf8 ("saving " + std::to_string (number));
    std::filesystem::remove_all (copying, error);

    const auto copy = [&copying, &problem] (std::string_view relative, std::string_view bytes)
    {
        const auto to = copying / pathFromUtf8 (relative);
        std::error_code folderError;
        std::filesystem::create_directories (to.parent_path(), folderError);
        const auto written = folderError ? folderError.message() : platform::writeFileToDisk (to, bytes);

        if (! written.empty())
            problem = "couldn't write its copy of " + std::string (relative) + ": " + withoutStop (written);

        return written.empty();
    };

    auto copied = copy (projectFileName, *projectText);

    for (const auto& file : files)
    {
        if (! copied)
            break;

        const auto path = folder / pathFromUtf8 (file);

        if (! std::filesystem::exists (path, error))
            continue;

        const auto text = readTextFile (path);

        if (! text)
        {
            problem = "couldn't read " + file;
            copied = false;
            break;
        }

        copied = copy (file, *text);
    }

    if (copied)
        copied = copy (saveInfoFile, info && ! infoTextKept.empty() ? infoTextKept : saveInfoText (*info));

    if (copied)
    {
        if (const auto moved = platform::moveFile (copying, savesPath / pathFromUtf8 (name)); ! moved.empty())
        {
            problem = "couldn't name its copy " + singleQuoted (name) + ": " + withoutStop (moved);
            copied = false;
        }
    }

    if (! copied)
    {
        std::filesystem::remove_all (copying, error);
        return {};
    }

    return commands::KeptSave { number, std::move (*info), std::string (keptSavesFolder) + "/" + name };
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
        std::optional<std::string> was;

        if (const auto problem = onDisk (path, file, was); ! problem.empty())
            return failed (problem);

        if (! was || *was != text)
            pending.push_back ({ file, &text, path, temporaryFor (path), std::move (was) });
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
        const auto& file = pending[i];
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
    }

    commands::SaveResult result { true };

    // The version this replaces, kept whole before anything changes, so no
    // save ever loses one (SPEC §4.5).
    if (projectChanged)
    {
        std::string problem;
        result.kept = keepVersionOnDisk (problem);

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

            return failed (message + (i == 0 ? " Nothing on disk changed" : " Nothing on disk changed: the files already replaced were put back")
                           + ". If another program has the file open, close it there, then save again");
        }

        if (file.file != saveInfoFile)
            result.written.push_back (file.file);
    }

    // Clip files that were in the project and are still here, but that no
    // clip uses now. project.json no longer names them, so moving them can't
    // break it.
    std::vector<std::string> notMoved;
    std::set<std::string, std::less<>> stillHere;

    for (const auto& file : clipFiles)
    {
        if (files.contains (file))
            continue;

        const auto path = folder / pathFromUtf8 (file);
        std::error_code error;

        if (! std::filesystem::exists (path, error))
            continue;

        const auto place = unusedPlace (folder, file);
        const auto destination = folder / pathFromUtf8 (place);
        std::filesystem::create_directories (destination.parent_path(), error);
        const auto problem = error ? error.message() : platform::moveFile (path, destination);

        if (! problem.empty())
        {
            notMoved.push_back (file + " (" + withoutStop (problem) + ")");
            stillHere.insert (file);
            continue;
        }

        result.moved.push_back ({ file, place });
    }

    clipFiles = std::move (stillHere);

    for (const auto& file : order)
        if (file != projectFileName)
            clipFiles.insert (file);

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
