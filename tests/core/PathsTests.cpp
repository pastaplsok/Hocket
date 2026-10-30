#include "core/Paths.h"
#include "core/project/Project.h"

#include <doctest/doctest.h>

#include <random>

using namespace hocket;

namespace
{
    // Greek and Japanese are outside every Western ANSI code page, so text that
    // went through one comes out as '?'.
    const std::string unicodeName = "ünï ελλ テスト";

    // A new folder in the temp directory, deleted when the test ends.
    struct TemporaryFolder
    {
        explicit TemporaryFolder (const std::string& utf8Name)
            : path (std::filesystem::temp_directory_path() / pathFromUtf8 (utf8Name + " " + std::to_string (std::random_device {}())))
        {
            std::filesystem::create_directories (path);
        }

        ~TemporaryFolder()
        {
            std::error_code error;
            std::filesystem::remove_all (path, error);
        }

        std::filesystem::path path;
    };

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }
}

TEST_CASE ("UTF-8 text and paths convert without loss")
{
    const auto path = pathFromUtf8 (unicodeName + "/clips/bass.notes");

    CHECK (path == std::filesystem::path (u8"ünï ελλ テスト/clips/bass.notes"));
    CHECK (utf8FromPath (path) == unicodeName + "/clips/bass.notes");

   #ifdef _WIN32
    CHECK (path.native() == L"ünï ελλ テスト/clips/bass.notes"); // what Windows itself sees
   #endif
}

TEST_CASE ("a project in a folder with a non-ASCII name loads, and errors name the folder correctly")
{
    const TemporaryFolder temporary (unicodeName);
    const auto projectFolder = temporary.path / pathFromUtf8 ("song");
    std::filesystem::copy (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo"), projectFolder, std::filesystem::copy_options::recursive);

    // The folder arrives as UTF-8 text, the way it does from the command line.
    const auto utf8Folder = utf8FromPath (projectFolder);
    const auto loaded = project::load (pathFromUtf8 (utf8Folder));

    for (const auto& error : loaded.errors)
        MESSAGE (project::formatDiagnostic (error));

    CHECK (loaded.project);

    const auto missing = project::load (pathFromUtf8 (utf8Folder + "/nope"));
    REQUIRE (missing.errors.size() == 1);
    CHECK (contains (missing.errors[0].message, unicodeName + " "));
    CHECK (contains (missing.errors[0].message, "/nope' isn't a folder"));
}
