#include "core/project/ClipFileWatcher.h"

#include "common/TemporaryFolder.h"

#include <doctest/doctest.h>

#include <fstream>

using namespace hocket;
using namespace std::chrono_literals;
using project::ClipFileWatcher;

namespace
{
    // The demo project, copied into a folder with a non-ASCII name.
    struct DemoProject
    {
        DemoProject()
        {
            std::filesystem::copy (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo"), folder, std::filesystem::copy_options::recursive);
            auto loaded = project::load (folder);
            REQUIRE (loaded.project);
            project = std::move (*loaded.project);
        }

        testing::TemporaryFolder temporary { testing::unicodeName };
        std::filesystem::path folder = temporary.path / pathFromUtf8 ("song");
        std::filesystem::path bass = folder / pathFromUtf8 ("clips/bass.notes");
        project::Project project;
    };

    void writeFile (const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream out (path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    // Each a different length, so the change shows even within the file system's timestamp resolution.
    const std::string oneNote = "@length 2bars\n\n1.1 C2 1/4 v100\n";
    const std::string twoNotes = "@length 2bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n";
    const std::string threeNotes = "@length 2bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n1.3 G2 1/4 v80\n";
}

TEST_CASE ("the clip file watcher reports a change once it has settled, once")
{
    DemoProject demo;
    const auto start = ClipFileWatcher::Clock::now();
    ClipFileWatcher watcher (demo.folder, demo.project);

    CHECK (watcher.poll (start).empty()); // the files as they were at the start aren't changes

    writeFile (demo.bass, twoNotes);
    CHECK (watcher.poll (start + 50ms).empty());  // just seen
    CHECK (watcher.poll (start + 150ms).empty()); // 100 ms quiet: not settled yet

    const auto changes = watcher.poll (start + 200ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].clipId == "clp-bass");
    CHECK (changes[0].file == "clips/bass.notes");
    CHECK (changes[0].text == twoNotes);

    CHECK (watcher.poll (start + 300ms).empty());
}

TEST_CASE ("the clip file watcher waits for a file that's saved in several steps")
{
    DemoProject demo;
    const auto start = ClipFileWatcher::Clock::now();
    ClipFileWatcher watcher (demo.folder, demo.project);

    writeFile (demo.bass, oneNote);
    CHECK (watcher.poll (start).empty());
    writeFile (demo.bass, twoNotes);
    CHECK (watcher.poll (start + 100ms).empty());
    writeFile (demo.bass, threeNotes);
    CHECK (watcher.poll (start + 200ms).empty());
    CHECK (watcher.poll (start + 300ms).empty());

    const auto changes = watcher.poll (start + 350ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].text == threeNotes);
}

TEST_CASE ("the clip file watcher sees a save through a temporary file and a rename as one change")
{
    DemoProject demo;
    const auto start = ClipFileWatcher::Clock::now();
    ClipFileWatcher watcher (demo.folder, demo.project);
    const auto temporaryFile = demo.folder / pathFromUtf8 ("clips/bass.notes.tmp");

    writeFile (temporaryFile, twoNotes);
    std::filesystem::remove (demo.bass);
    CHECK (watcher.poll (start).empty()); // caught mid-save, with the file missing

    std::filesystem::rename (temporaryFile, demo.bass);
    CHECK (watcher.poll (start + 50ms).empty());

    const auto changes = watcher.poll (start + 250ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].text == twoNotes);
}

TEST_CASE ("the clip file watcher reports a deleted clip file with no text")
{
    DemoProject demo;
    const auto start = ClipFileWatcher::Clock::now();
    ClipFileWatcher watcher (demo.folder, demo.project);

    std::filesystem::remove (demo.bass);
    CHECK (watcher.poll (start).empty());

    const auto changes = watcher.poll (start + 150ms);
    REQUIRE (changes.size() == 1);
    CHECK_FALSE (changes[0].text);
}
