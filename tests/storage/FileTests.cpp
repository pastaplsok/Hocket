// The platform layer's file writes that saving is built on (SPEC §4.5), in a
// folder with a non-ASCII name (CLAUDE.md).

#include "common/TemporaryFolder.h"
#include "core/Paths.h"
#include "platform/Platform.h"

#include <doctest/doctest.h>

#include <fstream>

using namespace hocket;

namespace
{
    std::filesystem::path in (const testing::TemporaryFolder& folder, const std::string& name)
    {
        return folder.path / pathFromUtf8 (name);
    }
}

TEST_CASE ("writeFileToDisk writes the bytes exactly, replacing any file of that name")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto file = in (folder, "über " + testing::unicodeName + ".notes");
    const std::string bytes ("line one\r\nline two\n\0 and after a zero byte", 42);

    REQUIRE (platform::writeFileToDisk (file, "something longer than what replaces it, to see it's cut short").empty());
    REQUIRE (platform::writeFileToDisk (file, bytes).empty());
    CHECK (readTextFile (file) == bytes);

    SUBCASE ("an empty file")
    {
        REQUIRE (platform::writeFileToDisk (file, "").empty());
        CHECK (readTextFile (file) == "");
    }
}

TEST_CASE ("writeFileToDisk fails in a folder that isn't there, and leaves nothing")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto file = in (folder, "missing/file.notes");

    CHECK_FALSE (platform::writeFileToDisk (file, "text").empty());
    CHECK_FALSE (std::filesystem::exists (file));
}

TEST_CASE ("replaceFile puts one file in place of another, or where none was")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto from = in (folder, "neu " + testing::unicodeName);
    const auto to = in (folder, "alt " + testing::unicodeName);

    REQUIRE (platform::writeFileToDisk (from, "new").empty());

    SUBCASE ("replacing")
    {
        REQUIRE (platform::writeFileToDisk (to, "old text").empty());
        CHECK (platform::replaceFile (from, to).empty());
    }

    SUBCASE ("where none was")
    {
        CHECK (platform::replaceFile (from, to).empty());
    }

    CHECK (readTextFile (to) == "new");
    CHECK_FALSE (std::filesystem::exists (from));
}

TEST_CASE ("moveFile never moves a file over another")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto from = in (folder, "a " + testing::unicodeName);
    const auto to = in (folder, "b " + testing::unicodeName);

    REQUIRE (platform::writeFileToDisk (from, "a").empty());

    SUBCASE ("a file is there")
    {
        REQUIRE (platform::writeFileToDisk (to, "b").empty());
        CHECK_FALSE (platform::moveFile (from, to).empty());
        CHECK (readTextFile (from) == "a");
        CHECK (readTextFile (to) == "b");
    }

    SUBCASE ("nothing is there")
    {
        CHECK (platform::moveFile (from, to).empty());
        CHECK_FALSE (std::filesystem::exists (from));
        CHECK (readTextFile (to) == "a");
    }
}

#ifdef _WIN32
TEST_CASE ("on Windows, replaceFile fails while another program has the file open without sharing deletes, and says why")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto from = in (folder, "new");
    const auto to = in (folder, "held");

    REQUIRE (platform::writeFileToDisk (from, "new").empty());
    REQUIRE (platform::writeFileToDisk (to, "old").empty());

    // The C runtime opens files sharing reads and writes, but not deletes,
    // as some editors do.
    std::ifstream held (to, std::ios::binary);
    REQUIRE (held);

    const auto problem = platform::replaceFile (from, to);

    CHECK_FALSE (problem.empty());
    MESSAGE ("Windows says: ", problem);
    held.close();
    CHECK (readTextFile (to) == "old");
    CHECK (readTextFile (from) == "new");
}
#endif
