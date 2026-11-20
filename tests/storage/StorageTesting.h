#pragma once

// Helpers for the storage tests: folders read whole, and a project saved in a
// folder of its own, opened as --play opens one.

#include "common/TemporaryFolder.h"
#include "core/Paths.h"
#include "core/commands/CommandBus.h"
#include "platform/Platform.h"
#include "storage/ProjectFolder.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace hocket::testing
{
    using namespace hocket::commands;

    inline const Author lee { "Lee" };
    inline const Author claude { "Claude", "session-1" };

    inline bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    // "clips/bass.notes": a file's path in the folder, with forward slashes.
    inline std::string relativeName (const std::filesystem::path& file, const std::filesystem::path& folder)
    {
        auto name = utf8FromPath (file.lexically_relative (folder));
        std::ranges::replace (name, '\\', '/');
        return name;
    }

    // Every file under the folder, by relativeName, and its bytes.
    inline std::map<std::string, std::string> filesIn (const std::filesystem::path& folder)
    {
        std::map<std::string, std::string> files;

        for (const auto& entry : std::filesystem::recursive_directory_iterator (folder))
            if (entry.is_regular_file())
                files[relativeName (entry.path(), folder)] = *readTextFile (entry.path());

        return files;
    }

    // Not in .hocket/ or unused/: the project's own.
    inline bool isProjectFile (const std::string& name)
    {
        return ! name.starts_with (".hocket/") && ! name.starts_with ("unused/");
    }

    // filesIn, but only the project's own files.
    inline std::map<std::string, std::string> projectFilesIn (const std::filesystem::path& folder)
    {
        auto files = filesIn (folder);
        std::erase_if (files, [] (const auto& file) { return ! isProjectFile (file.first); });
        return files;
    }

    // The project's own files' modified times.
    inline std::map<std::string, std::filesystem::file_time_type> modifiedTimes (const std::filesystem::path& folder)
    {
        std::map<std::string, std::filesystem::file_time_type> times;

        for (const auto& entry : std::filesystem::recursive_directory_iterator (folder))
            if (const auto name = relativeName (entry.path(), folder); entry.is_regular_file() && isProjectFile (name))
                times[name] = entry.last_write_time();

        return times;
    }

    using namespace std::chrono_literals;

    // When the tests' saves happen: the first, then the bus's, unless a test moves it.
    inline const SaveTime firstSaved = std::chrono::sys_days (std::chrono::year (2026) / 11 / 20) + 9h + 15min;

    inline project::Project loaded (const std::filesystem::path& folder)
    {
        auto result = project::load (folder);

        for (const auto& error : result.errors)
            MESSAGE (project::formatDiagnostic (error));

        REQUIRE (result.project);
        return std::move (*result.project);
    }

    inline void writeFile (const std::filesystem::path& file, const std::string& text)
    {
        std::filesystem::create_directories (file.parent_path());
        REQUIRE (platform::writeFileToDisk (file, text).empty());
    }

    // A project saved in a folder of its own by Lee, at firstSaved, and
    // opened as --play opens one: a bus, with the folder as its storage, whose
    // saves happen at `now`. The drum fixture's: two tracks, three clip
    // files, one of them linked.
    struct SavedProject
    {
        SavedProject()
        {
            const auto fixture = loaded (pathFromUtf8 (HOCKET_FIXTURES_DIR "/drum-project"));
            storage::ProjectFolder first (folder, project::Project {}, [] { return std::chrono::system_clock::time_point (firstSaved); });
            REQUIRE (first.save (fixture, {}, lee).ok);
            open();
        }

        // Loads the folder again, as a new session would.
        void open()
        {
            bus.reset();
            auto [result, opened] = storage::ProjectFolder::open (folder, [this] { return now; });

            for (const auto& error : result.errors)
                MESSAGE (project::formatDiagnostic (error));

            REQUIRE (opened);
            projectFolder.emplace (std::move (*opened));
            bus.emplace (std::move (*result.project));
            bus->setStorage (&*projectFolder);
        }

        std::filesystem::path file (const std::string& relative) const { return folder / pathFromUtf8 (relative); }

        TemporaryFolder temporary { unicodeName };
        std::filesystem::path folder = temporary.path / pathFromUtf8 ("song " + unicodeName);
        std::optional<storage::ProjectFolder> projectFolder;
        std::optional<CommandBus> bus;
        std::chrono::system_clock::time_point now = firstSaved + 2h;
    };
}
