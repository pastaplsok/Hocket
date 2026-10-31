#include "core/Paths.h"
#include "core/project/Project.h"

#include "common/TemporaryFolder.h"

#include <doctest/doctest.h>

using namespace hocket;
using hocket::testing::TemporaryFolder;
using hocket::testing::unicodeName;

namespace
{
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

#ifdef _WIN32
TEST_CASE ("a Windows name with an unpaired surrogate comes out as UTF-8, with U+FFFD in its place")
{
    // NTFS names are UTF-16 that needn't be valid: nothing types these, but
    // some programs make them. A folder like this is a working folder like any other.
    const std::filesystem::path path (std::wstring (L"song ") + wchar_t (0xD800) + L" mix/" + wchar_t (0xDC00) + L"a");

    std::string utf8;
    CHECK_NOTHROW (utf8 = utf8FromPath (path));
    CHECK (utf8 == "song \xEF\xBF\xBD mix/\xEF\xBF\xBD" "a");

    // A valid pair is still one character.
    CHECK (utf8FromPath (std::filesystem::path (std::wstring { wchar_t (0xD83C), wchar_t (0xDFB5) })) == "\xF0\x9F\x8E\xB5");
}
#endif

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
