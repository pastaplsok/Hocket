// Changes made outside Hocket (SPEC §4.5): a save never overwrites one
// unasked, --watch takes them in as the person's commands, and Hocket's own
// saves are never taken for them.

#include "StorageTesting.h"

#include "storage/WatchedFolder.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;
using namespace std::chrono_literals;

namespace
{
    using Clock = storage::ProjectWatcher::Clock;

    const std::string newBass = "@length 4bars\n\n1.1 C2 1/2 v100\n";
    const std::string newFill = "@length 1bar\n\n1.1 snare 1/16 v90\n";
    const std::string byHand = "@length 4bars\n\n1.1    C2 1/4 v100\n"; // as an editor might leave it: not canonical

    // A version of the project as a folder holds it: not the kept saves or unused files.
    std::map<std::string, std::string> versionIn (const std::filesystem::path& folder)
    {
        auto files = filesIn (folder);
        std::erase_if (files, [] (const auto& file) { return file.first.starts_with (".hocket/saves/") || file.first.starts_with ("unused/"); });
        return files;
    }

    std::string folderTime (SaveTime time)
    {
        auto text = localTimeText (time);
        std::ranges::replace (text, ':', '-');
        return text;
    }

    // The session's project as Lee might save project.json by hand: a new tempo.
    void saveProjectJsonByHand (SavedProject& saved, double tempo)
    {
        auto edited = saved.bus->project();
        edited.tempo = tempo;
        writeFile (saved.file ("project.json"), project::write (edited).at ("project.json"));
    }

    const std::string refusedForBass = "didn't save: it would overwrite changes made since Hocket last read or wrote this file: clips/bass.notes changed outside "
                                       "Hocket. Nothing on disk changed. Someone may have edited it by hand: ask before saving over it. Saving with overwrite "
                                       "saves anyway, keeping the version on disk as a kept save first, so it can be got back";
}

TEST_CASE ("a save won't overwrite a clip file changed outside Hocket: it names it, and writes nothing")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    writeFile (saved.file ("clips/bass.notes"), byHand);
    const auto before = filesIn (saved.folder);

    const auto result = saved.bus->save ({}, claude);

    CHECK_FALSE (result.ok);
    CHECK (result.message == refusedForBass);
    CHECK (filesIn (saved.folder) == before);
    CHECK (saved.bus->hasUnsavedChanges());
}

TEST_CASE ("with overwrite, a save keeps the version on disk first, labelled as changed outside Hocket at the change's time")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    writeFile (saved.file ("clips/bass.notes"), byHand);

    const SaveTime changedAt = firstSaved + 1h;
    std::filesystem::last_write_time (saved.file ("clips/bass.notes"), std::chrono::clock_cast<std::chrono::file_clock> (changedAt));
    const auto onDisk = versionIn (saved.folder);

    const auto result = saved.bus->save ({ {}, true }, claude);

    REQUIRE (result.ok);
    REQUIRE (result.kept);
    CHECK (result.kept->info == SaveInfo { changedAt, "", "", true });
    CHECK (result.kept->folder == ".hocket/saves/1 " + folderTime (changedAt) + " changed outside Hocket");
    CHECK (describe (*result.kept) == "save #1, changed outside Hocket, " + localTimeText (changedAt));
    CHECK (result.message == "saved: wrote clips/bass.notes; the version it replaced is kept as " + describe (*result.kept));

    // The person's version is in the kept save, and listed as it was kept.
    auto kept = filesIn (saved.file (result.kept->folder));
    auto expected = onDisk;
    kept.erase (".hocket/save.json");
    expected.erase (".hocket/save.json");
    CHECK (kept == expected);
    CHECK (kept.at ("clips/bass.notes") == byHand);
    CHECK (saved.bus->keptSaves() == std::vector<KeptSave> { *result.kept });

    CHECK (readTextFile (saved.file ("clips/bass.notes")) == project::clipFileText (saved.bus->project(), "clips/bass.notes"));
    CHECK_FALSE (saved.bus->hasUnsavedChanges());

    // Saved over, the file is Hocket's again: the next save needs no overwrite.
    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    CHECK (saved.bus->save ({}, claude).ok);
}

TEST_CASE ("every kind of outside change a save would overwrite is named")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (bus.execute (WriteClip { "clp-fill", newFill }, claude).ok);
    REQUIRE (bus.execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);

    writeFile (saved.file ("clips/bass.notes"), byHand);
    std::filesystem::remove (saved.file ("clips/fill.notes"));
    writeFile (saved.file ("clips/verse.notes"), byHand); // where Hocket's new clip's file goes
    saveProjectJsonByHand (saved, 96);
    const auto before = filesIn (saved.folder);

    const auto result = bus.save ({}, claude);

    CHECK_FALSE (result.ok);
    CHECK (result.message
           == "didn't save: it would overwrite changes made since Hocket last read or wrote these files: clips/bass.notes changed outside Hocket; "
              "clips/fill.notes was deleted outside Hocket; clips/verse.notes is there already, but Hocket never had it in the project; project.json "
              "changed outside Hocket. Nothing on disk changed. Someone may have edited them by hand: ask before saving over them. Saving with overwrite "
              "saves anyway, keeping the version on disk as a kept save first, so it can be got back");
    CHECK (filesIn (saved.folder) == before);

    // With overwrite, the file put there by hand is kept too.
    const auto overwritten = bus.save ({ {}, true }, claude);
    REQUIRE (overwritten.kept);
    CHECK (readTextFile (saved.file (overwritten.kept->folder + "/clips/verse.notes")) == byHand);
    CHECK (loaded (saved.folder) == bus.project());
}

TEST_CASE ("with project.json gone, a save with overwrite still keeps every file it replaces first")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    writeFile (saved.file ("clips/bass.notes"), byHand);
    std::filesystem::remove (saved.file ("project.json")); // moved away, say, or a checkout

    CHECK_FALSE (saved.bus->save ({}, claude).ok);
    const auto result = saved.bus->save ({ {}, true }, claude);

    REQUIRE (result.ok);
    REQUIRE (result.kept);
    CHECK (result.kept->info.changedOutside);
    CHECK (readTextFile (saved.file (result.kept->folder + "/clips/bass.notes")) == byHand);
    CHECK_FALSE (std::filesystem::exists (saved.file (result.kept->folder + "/project.json")));
    CHECK (result.message == "saved: wrote clips/bass.notes and project.json; the version it replaced is kept as " + describe (*result.kept));
    CHECK (loaded (saved.folder) == saved.bus->project());
}

TEST_CASE ("a save that only adds files replaces nothing, so it keeps nothing")
{
    SavedProject saved;
    std::filesystem::remove (saved.file ("project.json"));
    std::filesystem::remove (saved.file ("clips/bass.notes"));
    std::filesystem::remove (saved.file ("clips/beat.notes"));
    std::filesystem::remove (saved.file ("clips/fill.notes"));

    const auto result = saved.bus->save ({ {}, true }, claude);

    REQUIRE (result.ok);
    CHECK_FALSE (result.kept);
    CHECK (saved.bus->keptSaves().empty());
    CHECK (loaded (saved.folder) == saved.bus->project());
}

TEST_CASE ("a file whose date changed, but whose text the save wouldn't change, doesn't stop it")
{
    SavedProject saved;
    const auto bass = saved.file ("clips/bass.notes");
    std::filesystem::last_write_time (bass, std::filesystem::last_write_time (bass) + 1h);
    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);

    const auto result = saved.bus->save ({}, claude);

    REQUIRE (result.ok);
    CHECK (result.written == std::vector<std::string> { "project.json" });
    CHECK (saved.projectFolder->knownStamp ("clips/bass.notes") == project::FileStamp::of (bass));
}

TEST_CASE ("changes in .hocket/ and unused/ never stop a save")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (RemoveClip { "clp-fill" }, claude).ok);
    const auto first = saved.bus->save ({}, claude);
    REQUIRE (first.kept);

    writeFile (saved.file (first.kept->folder + "/project.json"), "edited by hand");
    writeFile (saved.file (".hocket/save.json"), "edited by hand");
    writeFile (saved.file ("unused/clips/fill.notes"), "edited by hand");
    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);

    CHECK (saved.bus->save ({}, claude).ok);
}

TEST_CASE ("Hocket knows each project file by the stamp it has on disk: as opened, and as each save leaves it")
{
    SavedProject saved;
    const auto check = [&saved]
    {
        for (const auto& [file, text] : project::write (saved.bus->project()))
        {
            CAPTURE (file);
            CHECK (saved.projectFolder->knownStamp (file) == project::FileStamp::of (saved.file (file)));
            CHECK (saved.projectFolder->knownText (file) == text);
        }
    };

    check();

    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (saved.bus->execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);
    REQUIRE (saved.bus->save ({}, claude).ok);
    check();
}

//==============================================================================
TEST_CASE ("--watch takes a clip file saved outside Hocket in as the person's write_clip, and the next save doesn't count it as changed outside")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    CHECK (watched.poll (start).empty());

    writeFile (saved.file ("clips/bass.notes"), byHand);
    CHECK (watched.poll (start + 50ms).empty());
    const auto reports = watched.poll (start + 250ms);

    REQUIRE (reports.size() == 1);
    CHECK (reports[0].file == "clips/bass.notes");
    CHECK (reports[0].ok);
    CHECK (reports[0].message == "#1 wrote clip clp-bass (1 note)");
    CHECK_FALSE (reports[0].warning);
    CHECK (bus.history().front().author == lee);

    REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
    const auto result = bus.save ({}, claude);
    REQUIRE (result.ok);
    CHECK (result.written == std::vector<std::string> { "clips/bass.notes", "project.json" }); // written in canonical form
}

TEST_CASE ("--watch takes project.json saved outside Hocket in as one command replacing the project, with the session's unsaved clip changes")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    CHECK (watched.poll (start).empty());

    // Claude's unsaved change to the fill, then Lee's project.json: a new
    // tempo, and a new clip whose file Lee wrote too.
    REQUIRE (bus.execute (WriteClip { "clp-fill", newFill }, claude).ok);
    const auto claudesFill = *project::clipFileText (bus.project(), "clips/fill.notes");
    const auto before = bus.project();

    auto edited = bus.project();
    edited.tempo = 96;
    edited.tracks[1].clips.push_back ({ "clp-bass-2", "clips/bass-2.notes", 4 * 3840 });
    edited.notesFiles["clips/bass-2.notes"] = *notes::parse (newBass).clip;
    const auto files = project::write (edited);
    writeFile (saved.file ("clips/bass-2.notes"), files.at ("clips/bass-2.notes"));
    writeFile (saved.file ("project.json"), files.at ("project.json"));

    CHECK (watched.poll (start + 50ms).empty());
    const auto reports = watched.poll (start + 300ms);

    REQUIRE (reports.size() == 1);
    CHECK (reports[0].file == "project.json");
    CHECK (reports[0].ok);
    CHECK (reports[0].message == "#2 project.json saved outside Hocket. Changes: the tempo (174 BPM to 96 BPM); changed trk-bass");
    CHECK_FALSE (reports[0].warning); // Claude's unsaved change is to the fill's notes, which stay
    CHECK (bus.project() == edited);
    CHECK (project::clipFileText (bus.project(), "clips/fill.notes") == claudesFill);
    CHECK (bus.history().front().author == lee);
    CHECK (watched.poll (start + 600ms).empty());

    // One entry, undone and redone whole.
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project() == before);
    REQUIRE (bus.redo (lee).ok);

    // Both files were read, so the next save writes over neither as an outside change.
    CHECK (bus.save ({}, lee).ok);
    CHECK (loaded (saved.folder) == edited);
}

TEST_CASE ("--watch's project.json warns when it replaces a change to the project that wasn't saved, naming it and the entry to undo")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    CHECK (watched.poll (start).empty());

    // Claude adds a clip with notes, unsaved, so on no disk; then Lee changes
    // the tempo in project.json as it is on disk.
    REQUIRE (bus.execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);
    const auto withClaudesClip = bus.project();
    auto text = *readTextFile (saved.file ("project.json"));
    text.replace (text.find ("\"tempo\": 174"), 12, "\"tempo\": 96");
    writeFile (saved.file ("project.json"), text);

    CHECK (watched.poll (start + 50ms).empty());
    const auto reports = watched.poll (start + 300ms);

    REQUIRE (reports.size() == 1);
    CHECK (reports[0].ok);
    CHECK (reports[0].warning.value_or ("")
           == "warning: this save of project.json replaced changes made to the project since it was last loaded or saved (the latest: #1 \"added clip "
              "clp-verse on trk-bass at 5.1 (1 note)\" by Claude). They weren't saved, so the file didn't have them. Undoing #2 brings them back.");
    CHECK (project::findClip (bus.project(), "clp-verse") == nullptr);

    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project() == withClaudesClip);
}

TEST_CASE ("--watch reads project.json with a clip file saved outside at the same time, and takes both in at once")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    CHECK (watched.poll (start).empty());

    writeFile (saved.file ("clips/bass.notes"), byHand);
    saveProjectJsonByHand (saved, 96);

    CHECK (watched.poll (start + 50ms).empty());
    const auto reports = watched.poll (start + 300ms);

    REQUIRE (reports.size() == 1);
    CHECK (reports[0].file == "project.json");
    CHECK (bus.project().tempo == 96);
    CHECK (project::notesOf (bus.project(), *project::findClip (bus.project(), "clp-bass")) == *notes::parse (byHand).clip);
    CHECK (bus.history().size() == 1);
    CHECK (watched.poll (start + 600ms).empty());
}

TEST_CASE ("--watch reports a project.json that doesn't read once; the session carries on, and a save won't overwrite it unasked")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    const auto before = bus.project();
    CHECK (watched.poll (start).empty());

    writeFile (saved.file ("project.json"), "{ \"format\": \"hocket-project\", oops");
    CHECK (watched.poll (start + 50ms).empty());
    const auto reports = watched.poll (start + 300ms);

    REQUIRE (reports.size() == 1);
    CHECK_FALSE (reports[0].ok);
    CHECK (reports[0].message.starts_with ("project.json was saved outside Hocket, but it doesn't read, so the session carries on as it was:\n  project.json:1:"));
    CHECK (bus.project() == before);
    CHECK (bus.history().empty());
    CHECK (watched.poll (start + 1000ms).empty());

    REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
    const auto refused = bus.save ({}, claude);
    CHECK_FALSE (refused.ok);
    CHECK (contains (refused.message, "project.json changed outside Hocket"));

    const auto overwritten = bus.save ({ {}, true }, claude);
    REQUIRE (overwritten.kept);
    CHECK (readTextFile (saved.file (overwritten.kept->folder + "/project.json")) == "{ \"format\": \"hocket-project\", oops");
    CHECK (watched.poll (start + 1100ms).empty());
    CHECK (watched.poll (start + 1500ms).empty());
}

TEST_CASE ("--watch's clip save warns when it replaces a change that wasn't saved, and not when it was")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    CHECK (watched.poll (start).empty());
    REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);

    SUBCASE ("saved first: the file had it")
    {
        REQUIRE (bus.save ({}, claude).ok);
        CHECK (watched.poll (start + 10ms).empty());
        writeFile (saved.file ("clips/bass.notes"), byHand);
        CHECK (watched.poll (start + 50ms).empty());
        const auto reports = watched.poll (start + 300ms);
        REQUIRE (reports.size() == 1);
        CHECK (reports[0].ok);
        CHECK_FALSE (reports[0].warning);
    }

    SUBCASE ("not saved: the file didn't have it")
    {
        writeFile (saved.file ("clips/bass.notes"), byHand);
        CHECK (watched.poll (start + 50ms).empty());
        const auto reports = watched.poll (start + 300ms);
        REQUIRE (reports.size() == 1);
        CHECK (reports[0].ok);
        REQUIRE (reports[0].warning);
        CHECK (contains (*reports[0].warning, "this save replaced changes made to clips/bass.notes since it was last loaded or saved"));
    }
}

//==============================================================================
TEST_CASE ("--watch never takes Hocket's own saves for outside changes: project.json and clip files, between polls and while a change settles")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    storage::WatchedFolder watched (bus, *saved.projectFolder, lee);
    const auto start = Clock::now();
    const auto bass = saved.file ("clips/bass.notes");
    CHECK (watched.poll (start).empty());

    const auto quietUntil = [&watched, start] (std::chrono::milliseconds from, std::chrono::milliseconds to)
    {
        for (auto at = from; at <= to; at += 25ms)
        {
            CAPTURE (at.count());
            CHECK (watched.poll (start + at).empty());
        }
    };

    SUBCASE ("a save between polls, of project.json, a clip file and a new clip's file")
    {
        REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
        REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);
        REQUIRE (bus.execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);
        const auto result = bus.save ({}, claude);
        REQUIRE (result.ok);
        CHECK (result.written == std::vector<std::string> { "clips/bass.notes", "clips/verse.notes", "project.json" });

        quietUntil (10ms, 3000ms);
        CHECK (bus.history().size() == 3); // nothing came back in as Lee's
    }

    SUBCASE ("a second save while --watch is still settling the first one's writes")
    {
        REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
        REQUIRE (bus.save ({}, claude).ok);
        CHECK (watched.poll (start + 10ms).empty()); // sees the files change, and waits for them to settle

        REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);
        REQUIRE (bus.execute (SetTempo { 110 }, claude).ok);
        REQUIRE (bus.save ({}, claude).ok);

        quietUntil (60ms, 1000ms);
        CHECK (bus.history().size() == 3);
    }

    SUBCASE ("a save with overwrite while --watch is settling an outside change to the same file")
    {
        writeFile (bass, byHand); // Lee, in an editor
        CHECK (watched.poll (start + 10ms).empty());

        REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
        const auto result = bus.save ({ {}, true }, claude); // Claude saves over it, keeping Lee's version
        REQUIRE (result.kept);
        CHECK (result.kept->info.changedOutside);

        // The file is Hocket's now; Lee's version is in the kept save, not taken in.
        quietUntil (35ms, 1000ms);
        CHECK (bus.history().size() == 1);
    }

    SUBCASE ("a save refused while --watch is settling an outside change leaves it to --watch")
    {
        writeFile (bass, byHand);
        CHECK (watched.poll (start + 10ms).empty());

        REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
        CHECK (bus.save ({}, claude).message == refusedForBass);

        const auto reports = watched.poll (start + 300ms);
        REQUIRE (reports.size() == 1);
        CHECK (reports[0].file == "clips/bass.notes");
        CHECK (reports[0].ok);

        // Taken in, so it saves; and that save is Hocket's own.
        REQUIRE (bus.save ({}, claude).ok);
        quietUntil (325ms, 1000ms);
        CHECK (bus.history().size() == 2);
    }

    SUBCASE ("an outside change the moment after Hocket's save, to a file it just wrote, is still reported")
    {
        REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);
        const auto result = bus.save ({}, claude);
        REQUIRE (result.ok);
        CHECK (result.written == std::vector<std::string> { "clips/bass.notes" });
        writeFile (bass, byHand); // before --watch's next poll

        CHECK (watched.poll (start + 10ms).empty());
        const auto reports = watched.poll (start + 300ms);
        REQUIRE (reports.size() == 1);
        CHECK (reports[0].file == "clips/bass.notes");
        CHECK (reports[0].ok);
        CHECK_FALSE (reports[0].warning); // Claude's change was saved, so the file had it
        quietUntil (325ms, 1000ms);
    }
}
