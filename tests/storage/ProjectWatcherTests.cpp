// The watcher behind --play --watch (SPEC §4.5, §6.1): project files that
// differ on disk from what Hocket last read or wrote, once settled.

#include "StorageTesting.h"

#include "storage/ProjectWatcher.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;
using namespace std::chrono_literals;
using storage::ProjectWatcher;

namespace
{
    // Each a different length, so the change shows even within the file system's timestamp resolution.
    const std::string oneNote = "@length 4bars\n\n1.1 C2 1/4 v100\n";
    const std::string twoNotes = "@length 4bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n";
    const std::string threeNotes = "@length 4bars\n\n1.1 C2 1/4 v100\n1.2 Eb2 1/4 v90\n1.3 G2 1/4 v80\n";

    struct Watching
    {
        SavedProject saved;
        ProjectWatcher watcher { *saved.projectFolder };
        ProjectWatcher::Clock::time_point start = ProjectWatcher::Clock::now();
        std::filesystem::path bass = saved.file ("clips/bass.notes");

        std::vector<ProjectWatcher::Change> poll (std::chrono::milliseconds at) { return watcher.poll (saved.bus->project(), start + at); }
    };
}

TEST_CASE ("the watcher reports a change once it has settled, once")
{
    Watching w;

    CHECK (w.poll (0ms).empty()); // the files as Hocket has them aren't changes

    writeFile (w.bass, twoNotes);
    CHECK (w.poll (50ms).empty());  // just seen
    CHECK (w.poll (150ms).empty()); // 100 ms quiet: not settled yet

    const auto changes = w.poll (200ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].file == "clips/bass.notes");
    CHECK (changes[0].text == twoNotes);
    CHECK (changes[0].stamp == project::FileStamp::of (w.bass));

    CHECK (w.poll (300ms).empty());
}

TEST_CASE ("the watcher watches project.json too, and reports it first")
{
    Watching w;
    CHECK (w.poll (0ms).empty());

    writeFile (w.bass, twoNotes);
    auto projectJson = *readTextFile (w.saved.file ("project.json"));
    projectJson.replace (projectJson.find ("\"tempo\": 174"), 12, "\"tempo\": 96");
    writeFile (w.saved.file ("project.json"), projectJson);
    CHECK (w.poll (50ms).empty());

    const auto changes = w.poll (250ms);
    REQUIRE (changes.size() == 2);
    CHECK (changes[0].file == "project.json");
    CHECK (changes[0].text == projectJson);
    CHECK (changes[1].file == "clips/bass.notes");
}

TEST_CASE ("a file that linked clips share is watched once")
{
    Watching w;
    REQUIRE (w.saved.bus->project().notesFiles.size() == 3); // clp-beat and clp-beat-2 share clips/beat.notes

    writeFile (w.saved.file ("clips/beat.notes"), oneNote);
    CHECK (w.poll (50ms).empty());

    const auto changes = w.poll (200ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].file == "clips/beat.notes");
}

TEST_CASE ("the watcher waits for a file that's saved in several steps")
{
    Watching w;

    writeFile (w.bass, oneNote);
    CHECK (w.poll (0ms).empty());
    writeFile (w.bass, twoNotes);
    CHECK (w.poll (100ms).empty());
    writeFile (w.bass, threeNotes);
    CHECK (w.poll (200ms).empty());
    CHECK (w.poll (300ms).empty());

    const auto changes = w.poll (350ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].text == threeNotes);
}

TEST_CASE ("the watcher sees a save through a temporary file and a rename as one change")
{
    Watching w;
    const auto temporaryFile = w.saved.file ("clips/bass.notes.tmp");

    writeFile (temporaryFile, twoNotes);
    std::filesystem::remove (w.bass);
    CHECK (w.poll (0ms).empty()); // caught mid-save, with the file missing

    std::filesystem::rename (temporaryFile, w.bass);
    CHECK (w.poll (50ms).empty());

    const auto changes = w.poll (250ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].text == twoNotes);
}

#ifdef _WIN32
TEST_CASE ("the watcher tries again while a saved file can't be read yet, then gives up after a while")
{
    Watching w;
    writeFile (w.bass, twoNotes);
    CHECK (w.poll (0ms).empty());

    SUBCASE ("readable again")
    {
        {
            // As an editor might, still holding the file once it has settled.
            // On Windows, nothing else can read a file held like this.
            const auto held = platform::FileLock::acquire (w.bass);
            REQUIRE (held.has_value());

            CHECK (w.poll (200ms).empty());
            CHECK (w.poll (1000ms).empty());
        }

        const auto changes = w.poll (1100ms);
        REQUIRE (changes.size() == 1);
        CHECK (changes[0].text == twoNotes);
    }

    SUBCASE ("still held")
    {
        const auto held = platform::FileLock::acquire (w.bass);
        REQUIRE (held.has_value());
        CHECK (w.poll (1000ms).empty());

        const auto changes = w.poll (std::chrono::duration_cast<std::chrono::milliseconds> (ProjectWatcher::defaultSettleTime + ProjectWatcher::readRetryTime));
        REQUIRE (changes.size() == 1);
        CHECK_FALSE (changes[0].text);
        CHECK (w.poll (10000ms).empty()); // reported once
    }
}
#endif

TEST_CASE ("the watcher reports a deleted clip file with no text")
{
    Watching w;

    std::filesystem::remove (w.bass);
    CHECK (w.poll (0ms).empty());

    const auto changes = w.poll (150ms);
    REQUIRE (changes.size() == 1);
    CHECK_FALSE (changes[0].text);
    CHECK_FALSE (changes[0].stamp.exists);
}

TEST_CASE ("a change the session doesn't take in isn't reported again, until the file changes again")
{
    Watching w;

    writeFile (w.bass, twoNotes);
    CHECK (w.poll (0ms).empty());
    REQUIRE (w.poll (200ms).size() == 1); // and not noted as read: refused, say

    CHECK (w.poll (400ms).empty());
    CHECK (w.poll (5000ms).empty());

    writeFile (w.bass, threeNotes);
    CHECK (w.poll (5050ms).empty());
    const auto changes = w.poll (5250ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].text == threeNotes);
}

TEST_CASE ("a change the session takes in is noted, and isn't a change any more")
{
    Watching w;

    writeFile (w.bass, twoNotes);
    CHECK (w.poll (0ms).empty());
    const auto changes = w.poll (200ms);
    REQUIRE (changes.size() == 1);

    w.saved.projectFolder->noteRead (changes[0].file, changes[0].stamp, twoNotes);
    CHECK (w.poll (400ms).empty());

    // Put back as it was before: that differs from what Hocket has now.
    writeFile (w.bass, oneNote);
    CHECK (w.poll (450ms).empty());
    CHECK (w.poll (650ms).size() == 1);
}

TEST_CASE ("a clip file Hocket never had isn't watched until it's saved")
{
    Watching w;
    REQUIRE (w.saved.bus->execute (AddClip { "trk-bass", "5.1", EmptyClip { 1 }, "clp-verse" }, lee).ok);
    const auto verse = w.saved.file ("clips/verse.notes");

    // Put there by hand before Hocket saved its own: not the session's change.
    writeFile (verse, oneNote);
    CHECK (w.poll (0ms).empty());
    CHECK (w.poll (1000ms).empty());

    // Saved over it with overwrite, it's Hocket's, and watched from then on.
    REQUIRE (w.saved.bus->save ({ {}, true }, lee).ok);
    CHECK (w.poll (1050ms).empty());
    CHECK (w.poll (1250ms).empty());

    writeFile (verse, twoNotes);
    CHECK (w.poll (1300ms).empty());
    const auto changes = w.poll (1500ms);
    REQUIRE (changes.size() == 1);
    CHECK (changes[0].file == "clips/verse.notes");
}
