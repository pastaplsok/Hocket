// Kept saves (SPEC §4.5): every save keeps the version it replaces, whole, in
// .hocket/saves/, named validly on Windows whatever the author or name hold;
// any one loads and restores by itself, by revert or by hand.

#include "StorageTesting.h"

#include "core/project/ClipFileWatcher.h"

#include <set>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    // A version of the project as a folder holds it: its files, and who saved
    // them, but not the kept saves or unused files.
    std::map<std::string, std::string> versionIn (const std::filesystem::path& folder)
    {
        auto files = filesIn (folder);
        std::erase_if (files, [] (const auto& file) { return file.first.starts_with (".hocket/saves/") || file.first.starts_with ("unused/"); });
        return files;
    }

    // "2026-11-20 09-15": a time as a kept save's folder name has it.
    std::string folderTime (SaveTime time)
    {
        auto text = localTimeText (time);
        std::ranges::replace (text, ':', '-');
        return text;
    }

    SaveTime secondsOf (std::chrono::system_clock::time_point time)
    {
        return std::chrono::floor<std::chrono::seconds> (time);
    }
}

TEST_CASE ("every save keeps the version it replaces, whole, and says so")
{
    SavedProject saved;
    const auto first = versionIn (saved.folder);

    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    const auto result = saved.bus->save ({}, claude);

    REQUIRE (result.ok);
    REQUIRE (result.kept);
    CHECK (result.kept->number == 1);
    CHECK (result.kept->info == SaveInfo { firstSaved, "Lee", "" });
    CHECK (result.kept->folder == ".hocket/saves/1 " + folderTime (firstSaved) + " Lee");
    CHECK (result.message == "saved: wrote project.json; the version it replaced is kept as save #1, by Lee, " + localTimeText (firstSaved));

    // project.json, every clip file (though only project.json changed), and who saved them.
    CHECK (filesIn (saved.file (result.kept->folder)) == first);
    CHECK (saved.bus->keptSaves() == std::vector<KeptSave> { *result.kept });

    // A change and a named save: Claude's version is kept next, and this
    // one carries its name until a later save keeps it.
    const auto second = versionIn (saved.folder);
    saved.now += std::chrono::hours (1);
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", "@length 4bars\n" }, lee).ok);
    const auto named = saved.bus->save ({ "before the bridge experiment" }, lee);

    REQUIRE (named.kept);
    CHECK (named.kept->number == 2);
    CHECK (named.kept->info == SaveInfo { firstSaved + std::chrono::hours (2), "Claude", "" });
    CHECK (filesIn (saved.file (named.kept->folder)) == second);

    saved.now += std::chrono::hours (1);
    REQUIRE (saved.bus->execute (SetTempo { 110 }, claude).ok);
    const auto third = saved.bus->save ({}, claude);

    REQUIRE (third.kept);
    CHECK (third.kept->info == SaveInfo { firstSaved + std::chrono::hours (3), "Lee", "before the bridge experiment" });
    CHECK (third.kept->folder == ".hocket/saves/3 " + folderTime (third.kept->info.time) + " Lee before the bridge experiment");
    CHECK (contains (third.message, "is kept as save #3, by Lee, " + localTimeText (third.kept->info.time) + ", \"before the bridge experiment\""));

    const auto kept = saved.bus->keptSaves();
    REQUIRE (kept.size() == 3);
    CHECK (kept[0].number == 1);
    CHECK (kept[1].number == 2);
    CHECK (kept[2] == *third.kept);
}

TEST_CASE ("a kept save is a project folder of its own: it loads, and restores by hand")
{
    SavedProject saved;
    const auto before = saved.bus->project();

    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    REQUIRE (saved.bus->execute (RemoveClip { "clp-fill" }, claude).ok);
    const auto result = saved.bus->save ({}, claude);
    REQUIRE (result.kept);
    REQUIRE (result.moved.size() == 1);

    CHECK (loaded (saved.file (result.kept->folder)) == before);

    // By hand: its files copied over the project's.
    for (const auto& [name, text] : versionIn (saved.file (result.kept->folder)))
        writeFile (saved.file (name), text);

    CHECK (loaded (saved.folder) == before);
}

TEST_CASE ("revert brings a kept save into the session, as unsaved changes; the next save keeps what it replaces")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    const auto first = bus.project();

    REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
    REQUIRE (bus.execute (RemoveTrack { "trk-bass" }, claude).ok);
    REQUIRE (bus.save ({}, claude).ok);
    const auto second = versionIn (saved.folder);
    REQUIRE (bus.execute (SetTempo { 140 }, claude).ok);

    const auto reverted = bus.execute (Revert { 1 }, lee);

    REQUIRE (reverted.ok);
    CHECK (contains (reverted.message, "reverted to save #1, by Lee, " + localTimeText (firstSaved) + ". Changes: the tempo (140 BPM to 174 BPM); added trk-bass"));
    CHECK (bus.project() == first);
    CHECK (bus.hasUnsavedChanges());
    CHECK (versionIn (saved.folder) == second); // nothing on disk changes until a save

    const auto result = bus.save ({}, lee);

    REQUIRE (result.kept);
    CHECK (result.kept->number == 2);
    CHECK (filesIn (saved.file (result.kept->folder)) == second);
    CHECK (loaded (saved.folder) == first);
    CHECK_FALSE (bus.hasUnsavedChanges());

    SUBCASE ("undone, it's the project as it was before")
    {
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        CHECK (bus.project().tempo == 140);
    }
}

TEST_CASE ("revert refuses a kept save that doesn't read, saying where")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    const auto result = saved.bus->save ({}, claude);
    REQUIRE (result.kept);
    writeFile (saved.file (result.kept->folder + "/clips/bass.notes"), "@length 4bars\n\n1.1 H2 1/4 v100\n");

    const auto reverted = saved.bus->execute (Revert { 1 }, lee);

    CHECK_FALSE (reverted.ok);
    CHECK (contains (reverted.message, "revert: save #1, by Lee, " + localTimeText (firstSaved) + " can't be read, in " + result.kept->folder + ": clips/bass.notes:3:"));
    CHECK (saved.bus->project().tempo == 100);
}

TEST_CASE ("a save that changes nothing keeps nothing; one with a name names the version on disk")
{
    SavedProject saved;
    const auto before = filesIn (saved.folder);

    const auto plain = saved.bus->save ({}, lee);
    CHECK_FALSE (plain.kept);
    CHECK (filesIn (saved.folder) == before);

    const auto named = saved.bus->save ({ "before the bridge" }, claude);

    REQUIRE (named.ok);
    CHECK_FALSE (named.kept);
    CHECK (named.written.empty());
    CHECK (named.message == "saved: the files on disk already matched the project, so this save only names them \"before the bridge\"");
    auto filesBefore = before;
    std::erase_if (filesBefore, [] (const auto& file) { return ! isProjectFile (file.first); });
    CHECK (projectFilesIn (saved.folder) == filesBefore);
    CHECK (contains (*readTextFile (saved.file (".hocket/save.json")), "\"name\": \"before the bridge\""));
    CHECK (saved.bus->keptSaves().empty());

    // The next save keeps them, under that name.
    REQUIRE (saved.bus->execute (SetTempo { 100 }, lee).ok);
    const auto next = saved.bus->save ({}, lee);

    REQUIRE (next.kept);
    CHECK (next.kept->info == SaveInfo { secondsOf (saved.now), "Claude", "before the bridge" });
}

TEST_CASE ("a version Hocket didn't save is kept as from before Hocket, at project.json's date")
{
    const TemporaryFolder temporary (unicodeName);
    const auto folder = temporary.path / pathFromUtf8 ("by hand " + unicodeName);
    std::filesystem::copy (pathFromUtf8 (HOCKET_FIXTURES_DIR "/drum-project"), folder, std::filesystem::copy_options::recursive);

    const SaveTime handMade = std::chrono::sys_days (std::chrono::year (2026) / 11 / 18) + std::chrono::hours (21) + std::chrono::minutes (5);
    std::filesystem::last_write_time (folder / pathFromUtf8 ("project.json"), std::chrono::clock_cast<std::chrono::file_clock> (handMade));
    const auto version = filesIn (folder);

    storage::ProjectFolder projectFolder (folder, loaded (folder), [] { return std::chrono::system_clock::time_point (firstSaved); });
    CommandBus bus (loaded (folder));
    bus.setStorage (&projectFolder);
    REQUIRE (bus.execute (SetTempo { 100 }, lee).ok);

    const auto result = bus.save ({}, lee);

    REQUIRE (result.kept);
    CHECK (result.kept->info == SaveInfo { handMade, "", "" });
    CHECK (result.kept->folder == ".hocket/saves/1 " + folderTime (handMade) + " before Hocket");
    CHECK (describe (*result.kept) == "save #1, from before Hocket, " + localTimeText (handMade));
    CHECK (contains (result.message, "the version it replaced is kept as save #1, from before Hocket, "));

    // Its files as they were, and a save.json saying where it came from.
    auto copy = filesIn (folder / pathFromUtf8 (result.kept->folder));
    const auto info = copy.extract (".hocket/save.json");
    REQUIRE_FALSE (info.empty());
    CHECK (contains (info.mapped(), "\"author\": \"\""));
    CHECK (copy == version);
    CHECK (bus.keptSaves() == std::vector<KeptSave> { *result.kept });
}

TEST_CASE ("kept saves' folder names are valid on Windows whatever the author or the save's name holds")
{
    const TemporaryFolder temporary (unicodeName);

    struct Case
    {
        std::string author;
        std::string name;
    };

    const Case cases[] = {
        { "Lee", "" },
        { "AC/DC: \"Back\" <in> Black|?*\\", "what? no*" },
        { "CON", "NUL" },
        { "COM1", "" },
        { "tab\there\nnewline\x01\x7f", "trailing dots..." },
        { "...", "   " },
        { "ünï ελλ テスト", "ブリッジの前 🎸" },
        { std::string (200, 'x'), std::string (200, 'y') },
        { "bad \xff\xfe utf-8 \xc3", "half an emoji \xf0\x9f, a surrogate \xed\xa0\x80" },
        { "🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸🎸", "" },
        { "", "" }, // from before Hocket
    };

    std::set<std::string> made;

    for (std::size_t i = 0; i < std::size (cases); ++i)
    {
        const auto& c = cases[i];
        CAPTURE (i);
        const auto name = storage::keptSaveFolderName (static_cast<int> (i) + 10, { firstSaved, c.author, c.name });
        CAPTURE (name);

        CHECK (name.starts_with (std::to_string (i + 10) + " " + folderTime (firstSaved)));
        CHECK (name.find_first_of ("<>:\"/\\|?*") == std::string::npos);
        CHECK (std::ranges::none_of (name, [] (char ch) { return static_cast<unsigned char> (ch) < 0x20 || ch == 0x7f; }));
        CHECK (name.back() != ' ');
        CHECK (name.back() != '.');
        CHECK (name.find ("  ") == std::string::npos);
        CHECK (utf8FromPath (pathFromUtf8 (name)) == name); // valid UTF-8, no half surrogate pair
        CHECK (pathFromUtf8 (name).native().size() <= storage::maxKeptSaveNameLength);

        // Windows takes it as it is: the folder made with it is listed with it.
        REQUIRE (std::filesystem::create_directory (temporary.path / pathFromUtf8 (name)));
        made.insert (name);
    }

    std::set<std::string> listed;

    for (const auto& entry : std::filesystem::directory_iterator (temporary.path))
        listed.insert (utf8FromPath (entry.path().filename()));

    CHECK (listed == made);
    CHECK (storage::keptSaveFolderName (3, { firstSaved, "", "" }) == "3 " + folderTime (firstSaved) + " before Hocket");
}

TEST_CASE ("a save by an author with a name Windows refuses keeps its version, and lists the author and name as they were")
{
    SavedProject saved;
    const Author odd { "AC/DC: \"Back\"", "session-2", "<drums>" };
    REQUIRE (saved.bus->execute (SetTempo { 100 }, odd).ok);
    REQUIRE (saved.bus->save ({ "what? \"no\" | yes*" }, odd).ok);
    REQUIRE (saved.bus->execute (SetTempo { 110 }, odd).ok);

    const auto result = saved.bus->save ({}, odd);

    REQUIRE (result.kept);
    CHECK (result.kept->info.author == "AC/DC: \"Back\" (<drums>)");
    CHECK (result.kept->info.name == "what? \"no\" | yes*");
    CHECK (saved.bus->keptSaves().back() == *result.kept);
    CHECK (loaded (saved.file (result.kept->folder)).tempo == 100);
}

TEST_CASE ("the list of kept saves skips anything that isn't one, and numbers carry on from the highest")
{
    SavedProject saved;
    auto& bus = *saved.bus;

    for (const double tempo : { 100.0, 110.0 })
    {
        REQUIRE (bus.execute (SetTempo { tempo }, lee).ok);
        REQUIRE (bus.save ({}, lee).kept);
    }

    auto kept = bus.keptSaves();
    REQUIRE (kept.size() == 2);

    // A copy a save left part-way, other things, and #1 removed by hand.
    std::filesystem::create_directories (saved.file (".hocket/saves/saving 3/clips"));
    std::filesystem::create_directories (saved.file (".hocket/saves/notes"));
    writeFile (saved.file (".hocket/saves/7 is a file, not a folder"), "");
    std::filesystem::remove_all (saved.file (kept[0].folder));

    // #2's save.json gone: it's listed as from before Hocket, at its project.json's date.
    std::filesystem::remove (saved.file (kept[1].folder + "/.hocket/save.json"));
    const auto date = std::filesystem::last_write_time (saved.file (kept[1].folder + "/project.json"));

    REQUIRE (bus.execute (SetTempo { 120 }, lee).ok);
    const auto next = bus.save ({}, lee);

    REQUIRE (next.kept);
    CHECK (next.kept->number == 3);

    kept = bus.keptSaves();
    REQUIRE (kept.size() == 2);
    CHECK (kept[0].number == 2);
    CHECK (kept[0].info == SaveInfo { secondsOf (std::chrono::clock_cast<std::chrono::system_clock> (date)), "", "" });
    CHECK (kept[1] == *next.kept);
}

TEST_CASE ("a save that can't keep the version on disk writes nothing")
{
    SavedProject saved;
    writeFile (saved.file (".hocket/saves"), "a file where the kept saves go");
    REQUIRE (saved.bus->execute (SetTempo { 100 }, lee).ok);
    const auto before = filesIn (saved.folder);

    const auto result = saved.bus->save ({}, lee);

    CHECK_FALSE (result.ok);
    CHECK (contains (result.message, "didn't save: couldn't keep the version on disk before replacing it: couldn't write its copy of project.json: "));
    CHECK (contains (result.message, ". Nothing on disk changed"));
    CHECK (filesIn (saved.folder) == before);
    CHECK (saved.bus->hasUnsavedChanges());
}

TEST_CASE ("loading and --watch ignore .hocket/ and unused/")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (RemoveClip { "clp-fill" }, lee).ok);
    REQUIRE (saved.bus->execute (SetTempo { 100 }, lee).ok);
    const auto result = saved.bus->save ({}, lee);
    REQUIRE (result.kept);

    using Clock = project::ClipFileWatcher::Clock;
    const auto start = Clock::now();
    project::ClipFileWatcher watcher (saved.folder, saved.bus->project());

    // Broken files where loading and the watcher never look.
    writeFile (saved.file (result.kept->folder + "/project.json"), "{ not json");
    writeFile (saved.file (result.kept->folder + "/clips/bass.notes"), "nonsense");
    writeFile (saved.file ("unused/clips/fill.notes"), "nonsense");
    writeFile (saved.file (".hocket/save.json"), "nonsense");

    // And a change to a clip file, which the watcher does see.
    writeFile (saved.file ("clips/bass.notes"), "@length 4bars\n");

    (void) watcher.poll (start);
    const auto changes = watcher.poll (start + std::chrono::seconds (1));

    REQUIRE (changes.size() == 1);
    CHECK (changes[0].file == "clips/bass.notes");

    auto project = saved.bus->project();
    project.notesFiles["clips/bass.notes"] = *notes::parse ("@length 4bars\n").clip;
    CHECK (project::write (loaded (saved.folder)) == project::write (project));
}
