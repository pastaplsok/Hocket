#include "storage/ProjectFolder.h"

#include "core/Paths.h"
#include "platform/Platform.h"

#include <optional>
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

ProjectFolder::ProjectFolder (std::filesystem::path projectFolder, const project::Project& loaded) : folder (std::move (projectFolder))
{
    for (const auto& entry : loaded.notesFiles)
        clipFiles.insert (entry.first);
}

commands::SaveResult ProjectFolder::save (const project::Project& project, const commands::Author&)
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

    for (const auto& file : order)
    {
        const auto& text = files.at (file);
        const auto path = folder / pathFromUtf8 (file);
        std::error_code error;
        std::optional<std::string> was;

        if (std::filesystem::exists (path, error))
        {
            was = readTextFile (path);

            if (! was)
                return failed ("couldn't read " + file + ", which is there, to compare it with the project: another program may be holding it, "
                               "or it isn't a file. Nothing on disk changed");

            if (*was == text)
                continue;
        }

        pending.push_back ({ file, &text, path, temporaryFor (path), std::move (was) });
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

    for (std::size_t i = 0; i < pending.size(); ++i)
    {
        const auto& file = pending[i];

        if (const auto problem = platform::replaceFile (file.temporary, file.path); ! problem.empty())
        {
            removeTemporaries (std::span (pending).subspan (i));
            const auto notPutBack = putBack (std::span (pending).first (i));
            const auto message = "couldn't replace " + file.file + ": " + withStop (problem);

            if (notPutBack.empty())
                return failed (message + (i == 0 ? " Nothing on disk changed" : " Nothing on disk changed: the files already replaced were put back")
                               + ". If another program has the file open, close it there, then save again");

            return failed (message + " Putting back the files already replaced failed for " + listed (notPutBack)
                           + ", so the folder is part new, part old: save again once the problem is fixed");
        }

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
        said.emplace_back ("the files on disk already matched the project, so nothing was written");

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
    const auto saved = projectFolder.save (empty, author);
    return saved.ok ? std::string() : saved.message;
}
}
