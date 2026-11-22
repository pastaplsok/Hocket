// The bug sweep's confirmations (area 1, saving and files; PROGRESS.md):
// each test shows a bug as it is, so it passes until its fix. Each fix turns
// its test into one of the fixed behaviour, failing first, and moves it to the
// test file it belongs in; this file goes once they're all fixed.

#include "StorageTesting.h"

#include "storage/SaveKeys.h"
#include "storage/WatchedFolder.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;
using namespace std::chrono_literals;

namespace
{
    const std::string newBass = "@length 4bars\n\n1.1 C2 1/2 v100\n";
    const std::string byHand = "@length 4bars\n\n1.1    C2 1/4 v100\n# Lee's edit by hand\n";

    bool anyFileHolds (const std::filesystem::path& folder, const std::string& text)
    {
        for (const auto& [name, bytes] : filesIn (folder))
            if (bytes == text)
                return true;

        return false;
    }

    std::string replaced (std::string text, const std::string& from, const std::string& to)
    {
        const auto at = text.find (from);
        REQUIRE (at != std::string::npos);
        return text.replace (at, from.size(), to);
    }
}

//==============================================================================
TEST_CASE ("T1: overwrite with project.json missing keeps nothing, so a clip file edited by hand is lost")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);

    writeFile (saved.file ("clips/bass.notes"), byHand);    // Lee's edit by hand
    std::filesystem::remove (saved.file ("project.json")); // and project.json gone (moved away, a checkout...)

    const auto refused = saved.bus->save ({}, claude);
    MESSAGE ("refused: " << refused.message);
    CHECK_FALSE (refused.ok);

    const auto result = saved.bus->save ({ {}, true }, claude);
    MESSAGE ("overwrite: " << result.message);
    CHECK (result.ok);
    CHECK_FALSE (result.kept);                              // nothing kept...
    CHECK_FALSE (anyFileHolds (saved.folder, byHand));      // ...and Lee's edit is nowhere on disk
    CHECK (saved.bus->keptSaves().empty());
}

//==============================================================================
TEST_CASE ("T2: hand edits made while Hocket was closed are kept under the last Hocket save's author and name")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (SetTempo { 100 }, lee).ok);
    REQUIRE (saved.bus->save ({ "the good take" }, lee).ok);

    // Hocket closed; the bass edited by hand; Hocket opened again.
    writeFile (saved.file ("clips/bass.notes"), byHand);
    saved.open();

    REQUIRE (saved.bus->execute (SetTempo { 110 }, claude).ok);
    const auto result = saved.bus->save ({}, claude);
    REQUIRE (result.ok);
    REQUIRE (result.kept);
    MESSAGE ("kept as: " << describe (*result.kept));

    const auto keptBass = readTextFile (saved.file (result.kept->folder + "/clips/bass.notes"));
    CHECK (keptBass == byHand);                       // the kept version holds the hand edit...
    CHECK (result.kept->info.author == "Lee");        // ...labelled as Lee's save
    CHECK (result.kept->info.name == "the good take"); // ...named "the good take"
    CHECK_FALSE (result.kept->info.changedOutside);
}

//==============================================================================
TEST_CASE ("T3: in a deep folder, every save with changes fails at keeping the version on disk (no long-path manifest)")
{
    TemporaryFolder temporary { "sweep-long" };
    auto folder = temporary.path;

    // Grow the project folder's path to 215 characters.
    while (folder.native().size() < 205)
        folder /= pathFromUtf8 ("deeper");

    folder /= pathFromUtf8 (std::string (215 - folder.native().size() - 1, 'x'));
    MESSAGE ("project folder path: " << folder.native().size() << " characters");

    const auto fixture = loaded (pathFromUtf8 (HOCKET_FIXTURES_DIR "/drum-project"));
    storage::ProjectFolder first (folder, project::Project {});
    const auto made = first.save (fixture, {}, lee);
    MESSAGE ("first save (nothing to keep): " << made.message);
    REQUIRE (made.ok);

    auto [result, opened] = storage::ProjectFolder::open (folder);
    REQUIRE (opened);
    CommandBus bus (std::move (*result.project));
    bus.setStorage (&*opened);
    REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);

    const auto saved = bus.save ({}, claude);
    MESSAGE ("second save: " << saved.message);
    REQUIRE (saved.ok);
    REQUIRE (saved.kept);

    const auto keptProject = folder / pathFromUtf8 (saved.kept->folder + "/project.json");
    MESSAGE ("kept project.json's path: " << keptProject.native().size() << " characters; readable: " << readTextFile (keptProject).has_value());

    const auto reverted = bus.execute (Revert { 1 }, claude);
    MESSAGE ("revert to #1: " << reverted.message);
    CHECK_FALSE (reverted.ok);

    // Deeper still: "saving 1" itself too long.
    auto deeper = folder.parent_path() / pathFromUtf8 (std::string (222 - folder.parent_path().native().size() - 1, 'y'));
    storage::ProjectFolder second (deeper, project::Project {});
    REQUIRE (second.save (fixture, {}, lee).ok);
    auto [result2, opened2] = storage::ProjectFolder::open (deeper);
    REQUIRE (opened2);
    CommandBus bus2 (std::move (*result2.project));
    bus2.setStorage (&*opened2);
    REQUIRE (bus2.execute (SetTempo { 100 }, claude).ok);
    const auto saved2 = bus2.save ({}, claude);
    MESSAGE ("at " << deeper.native().size() << " characters: " << saved2.message);

    std::error_code error;
    std::filesystem::remove_all (temporary.path, error);
}

//==============================================================================
TEST_CASE ("T4: a typed word starting \"so\" saves over a file changed outside Hocket before the refusal is read")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    writeFile (saved.file ("clips/bass.notes"), byHand);

    storage::SaveKeys keys (*saved.bus, lee);

    // --play's timer hands every key typed since the last tick to press(), in one loop.
    for (const auto key : std::u32string (U"sort"))
        if (const auto said = keys.press (key))
            MESSAGE ("key '" << static_cast<char> (key) << "': " << *said);

    CHECK (readTextFile (saved.file ("clips/bass.notes")) != byHand); // overwritten
    CHECK_FALSE (saved.bus->hasUnsavedChanges());
}

//==============================================================================
TEST_CASE ("T5: --watch's project.json replaces an unsaved API change with no warning")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = storage::ProjectWatcher::Clock::now();
    CHECK (watched.poll (start).empty());

    // Claude adds a clip with notes, unsaved: its notes are on no disk.
    REQUIRE (bus.execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);

    // Lee changes the tempo in project.json by hand, in the file as it is on disk.
    const auto onDisk = *readTextFile (saved.file ("project.json"));
    writeFile (saved.file ("project.json"), replaced (onDisk, "\"tempo\": 174", "\"tempo\": 96"));

    CHECK (watched.poll (start + 50ms).empty());
    const auto reports = watched.poll (start + 300ms);
    REQUIRE (reports.size() == 1);
    MESSAGE ("report: " << reports[0].message);
    CHECK (reports[0].ok);
    CHECK_FALSE (reports[0].warning);                                // no warning...
    CHECK (project::findClip (bus.project(), "clp-verse") == nullptr); // ...though Claude's clip is gone
}

//==============================================================================
TEST_CASE ("T6: a crash between the clip files' swap and project.json's leaves a version that's neither, and may not load")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (SetTimeSignature { "2/4" }, claude).ok);

    // What the save swaps in first: the clip files, but not yet project.json.
    for (const auto& [file, text] : project::write (saved.bus->project()))
        if (file != "project.json")
            writeFile (saved.file (file), text);

    const auto read = project::load (saved.folder);

    for (const auto& error : read.errors)
        MESSAGE (project::formatDiagnostic (error));

    CHECK_FALSE (read.project); // the folder doesn't load
}

//==============================================================================
TEST_CASE ("T7: a save with a name and no changes relabels the version on disk with the namer and the time of naming")
{
    SavedProject saved; // saved by Lee at firstSaved

    const auto named = saved.bus->save ({ "the take" }, claude);
    REQUIRE (named.ok);

    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    const auto result = saved.bus->save ({}, claude);
    REQUIRE (result.kept);
    MESSAGE ("Lee's version is kept as: " << describe (*result.kept));
    CHECK (result.kept->info.author == "Claude");
}
