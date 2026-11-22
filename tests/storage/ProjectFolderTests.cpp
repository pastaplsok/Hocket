// Saving a project into its folder (SPEC §4.5), through the command bus as
// clients do, in folders with non-ASCII names (CLAUDE.md).

#include "StorageTesting.h"

#include <fstream>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    const std::string newBass = "@length 4bars\n\n1.1 C2 1/2 v100\n";
    const std::string newFill = "@length 1bar\n\n1.1 snare 1/16 v90\n";
}

TEST_CASE ("a save writes each file of the project, clip files before project.json, and loading the folder gives the project back")
{
    const TemporaryFolder temporary (unicodeName);
    const auto folder = temporary.path / pathFromUtf8 ("new " + unicodeName);
    const auto project = loaded (pathFromUtf8 (HOCKET_FIXTURES_DIR "/drum-project"));

    storage::ProjectFolder projectFolder (folder, project::Project {});
    const auto saved = projectFolder.save (project, {}, lee);

    REQUIRE (saved.ok);
    CHECK (saved.message == "saved: wrote clips/bass.notes, clips/beat.notes, clips/fill.notes and project.json");
    CHECK (saved.written == std::vector<std::string> { "clips/bass.notes", "clips/beat.notes", "clips/fill.notes", "project.json" });
    CHECK (saved.moved.empty());
    CHECK_FALSE (saved.kept); // there was nothing to keep
    CHECK (projectFilesIn (folder) == project::write (project));
    CHECK (loaded (folder) == project);

    // Who saved it, and when, beside it.
    const auto info = readTextFile (folder / pathFromUtf8 (".hocket/save.json"));
    REQUIRE (info);
    CHECK (contains (*info, "\"format\": \"hocket-save\""));
    CHECK (contains (*info, "\"author\": \"Lee\""));
}

TEST_CASE ("save, load and save again: the second save writes nothing, and the files stay byte for byte the same")
{
    SavedProject saved;
    const auto before = filesIn (saved.folder);
    const auto times = modifiedTimes (saved.folder);

    const auto again = saved.bus->save ({}, lee);

    REQUIRE (again.ok);
    CHECK (again.message == "saved: the files on disk already matched the project, so nothing was written");
    CHECK (again.written.empty());
    CHECK (filesIn (saved.folder) == before);
    CHECK (modifiedTimes (saved.folder) == times);
}

TEST_CASE ("only the files whose text changed are written; the others keep their dates")
{
    SavedProject saved;
    const auto times = modifiedTimes (saved.folder);

    SUBCASE ("a clip's notes")
    {
        REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
        const auto result = saved.bus->save ({}, lee);

        REQUIRE (result.ok);
        CHECK (result.written == std::vector<std::string> { "clips/bass.notes" });
        CHECK (readTextFile (saved.file ("clips/bass.notes")) == project::clipFileText (saved.bus->project(), "clips/bass.notes"));

        auto after = modifiedTimes (saved.folder);
        after.erase ("clips/bass.notes");
        auto others = times;
        others.erase ("clips/bass.notes");
        CHECK (after == others);
    }

    SUBCASE ("the tempo")
    {
        REQUIRE (saved.bus->execute (SetTempo { 120 }, claude).ok);
        const auto result = saved.bus->save ({}, lee);

        REQUIRE (result.ok);
        CHECK (result.written == std::vector<std::string> { "project.json" });
    }

    SUBCASE ("a new clip: its file before project.json, in a folder made for it if need be")
    {
        std::filesystem::remove_all (saved.file ("clips"));
        saved.projectFolder.emplace (saved.folder, project::Project {}); // the clip files went outside Hocket
        REQUIRE (saved.bus->execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);

        const auto result = saved.bus->save ({}, lee);

        REQUIRE (result.ok);
        CHECK (result.written
               == std::vector<std::string> { "clips/bass.notes", "clips/beat.notes", "clips/fill.notes", "clips/verse.notes", "project.json" });
        CHECK (loaded (saved.folder) == saved.bus->project());
    }

    CHECK_FALSE (saved.bus->hasUnsavedChanges());
}

TEST_CASE ("a save that can't write a new file beside its own changes nothing, and leaves no temporary files")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (saved.bus->execute (WriteClip { "clp-fill", newFill }, claude).ok);

    // Something in the way of fill's temporary file; bass's is written first.
    const auto blocker = saved.file ("clips/fill.notes" + std::string (storage::temporarySuffix));
    std::filesystem::create_directories (blocker);
    const auto before = filesIn (saved.folder);

    const auto result = saved.bus->save ({}, lee);

    CHECK_FALSE (result.ok);
    CHECK (contains (result.message, "didn't save: couldn't write the new clips/fill.notes beside it, as clips/fill.notes.hocket-saving: "));
    CHECK (contains (result.message, "Nothing on disk changed"));
    CHECK (filesIn (saved.folder) == before);
    CHECK (saved.bus->hasUnsavedChanges());

    // Out of the way, the next save goes through.
    std::filesystem::remove (blocker);
    REQUIRE (saved.bus->save ({}, lee).ok);
    CHECK (loaded (saved.folder) == saved.bus->project());
}

TEST_CASE ("a save that finds a file it can't read changes nothing")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-fill", newFill }, claude).ok);
    const auto before = filesIn (saved.folder);

    {
        // Nothing else can even read it while this holds it (on Windows).
        const auto held = platform::FileLock::acquire (saved.file ("clips/fill.notes"));
        REQUIRE (held);

        const auto result = saved.bus->save ({}, lee);

        CHECK_FALSE (result.ok);
        CHECK (result.message
               == "didn't save: couldn't read clips/fill.notes, which is there, to compare it with the project: another program may be holding it, "
                  "or it isn't a file. Nothing on disk changed");
    }

    CHECK (filesIn (saved.folder) == before);
    CHECK (saved.bus->hasUnsavedChanges());
}

#ifdef _WIN32
TEST_CASE ("a save that can't replace a file puts back the files it already replaced: a failure mid-save leaves the old files")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (saved.bus->execute (WriteClip { "clp-fill", newFill }, claude).ok);
    const auto before = filesIn (saved.folder);

    {
        // Readable, but Windows won't replace it while it's open like this,
        // as some editors hold files. bass is replaced before fill.
        std::ifstream held (saved.file ("clips/fill.notes"), std::ios::binary);
        REQUIRE (held);

        const auto result = saved.bus->save ({}, lee);

        CHECK_FALSE (result.ok);
        CHECK (contains (result.message, "didn't save: couldn't replace clips/fill.notes: "));
        CHECK (contains (result.message, "Nothing on disk changed: the files already replaced were put back. "
                                         "If another program has the file open, close it there, then save again"));
        MESSAGE (result.message);
    }

    CHECK (filesIn (saved.folder) == before);
    CHECK (saved.bus->hasUnsavedChanges());

    // Once it's closed, the save goes through.
    const auto result = saved.bus->save ({}, lee);

    REQUIRE (result.ok);
    CHECK (result.written == std::vector<std::string> { "clips/bass.notes", "clips/fill.notes" });
    CHECK (loaded (saved.folder) == saved.bus->project());
}

TEST_CASE ("after a failed save that couldn't put a file back, Hocket still knows that file as its own")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (saved.bus->execute (WriteClip { "clp-fill", newFill }, claude).ok);

    // As fill goes in, another program holds fill, so it can't, and holds
    // bass, already in place, so it can't be put back.
    std::optional<std::ifstream> heldFill, heldBass;
    saved.projectFolder->setStepHook ([&] (const storage::ProjectFolder::SaveStep& step)
    {
        if (step.kind == storage::ProjectFolder::SaveStep::Kind::swap && step.path == saved.file ("clips/fill.notes"))
        {
            heldFill.emplace (saved.file ("clips/fill.notes"), std::ios::binary);
            heldBass.emplace (saved.file ("clips/bass.notes"), std::ios::binary);
        }

        return true;
    });

    const auto failed = saved.bus->save ({}, lee);
    heldFill.reset();
    heldBass.reset();
    saved.projectFolder->setStepHook ({});

    REQUIRE_FALSE (failed.ok);
    REQUIRE (contains (failed.message, "so the folder is part new, part old"));
    REQUIRE (*readTextFile (saved.file ("clips/bass.notes")) == project::write (saved.bus->project()).at ("clips/bass.notes"));

    // Hocket wrote the bass that stayed, so changing it again and saving
    // isn't refused as an outside change.
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", "@length 4bars\n\n1.1 C2 1/4 v90\n" }, claude).ok);
    const auto result = saved.bus->save ({}, lee);

    CHECK (result.ok);
    CHECK (result.outsideChanges.empty());
    CHECK (loaded (saved.folder) == saved.bus->project());
}

TEST_CASE ("a failed save takes away the new clip files it added")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);
    const auto before = filesIn (saved.folder);

    std::ifstream held (saved.file ("project.json"), std::ios::binary);
    REQUIRE (held);

    const auto result = saved.bus->save ({}, lee);

    CHECK_FALSE (result.ok);
    CHECK (contains (result.message, "couldn't replace project.json"));
    CHECK (filesIn (saved.folder) == before);
    CHECK_FALSE (std::filesystem::exists (saved.file ("clips/verse.notes")));
}
#endif

TEST_CASE ("clip files no clip uses any more move to unused/, keeping their paths, numbered when the name is taken, never deleted")
{
    SavedProject saved;
    auto& bus = *saved.bus;
    const auto fill = *readTextFile (saved.file ("clips/fill.notes"));

    // Files Hocket never had in the project, which no save touches.
    writeFile (saved.file ("clips/mine.notes"), "@length 1bar\n");
    writeFile (saved.file ("notes to self.txt"), "kick on 1");

    REQUIRE (bus.execute (RemoveClip { "clp-fill" }, claude).ok);
    const auto first = bus.save ({}, lee);

    REQUIRE (first.ok);
    CHECK (first.message == "saved: wrote project.json; the version it replaced is kept as save #1, by Lee, " + localTimeText (firstSaved)
                                + "; moved clips/fill.notes to unused/clips/fill.notes, since no clip uses it any more");
    CHECK (first.moved == std::vector<MovedFile> { { "clips/fill.notes", "unused/clips/fill.notes" } });
    CHECK_FALSE (std::filesystem::exists (saved.file ("clips/fill.notes")));
    CHECK (readTextFile (saved.file ("unused/clips/fill.notes")) == fill);

    // Loading ignores unused/.
    CHECK (loaded (saved.folder) == bus.project());

    // An undo brings the clip back: the next save writes its file in clips/
    // again, and the copy in unused/ stays.
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    const auto second = bus.save ({}, lee);

    REQUIRE (second.ok);
    CHECK (second.written == std::vector<std::string> { "clips/fill.notes", "project.json" });
    CHECK (second.moved.empty());
    CHECK (readTextFile (saved.file ("clips/fill.notes")) == fill);
    CHECK (readTextFile (saved.file ("unused/clips/fill.notes")) == fill);

    // Removed again, it's numbered, before its extension.
    REQUIRE (bus.redo (lee).ok);
    const auto third = bus.save ({}, lee);

    REQUIRE (third.ok);
    CHECK (third.moved == std::vector<MovedFile> { { "clips/fill.notes", "unused/clips/fill-2.notes" } });
    CHECK (readTextFile (saved.file ("unused/clips/fill-2.notes")) == fill);

    CHECK (readTextFile (saved.file ("clips/mine.notes")) == "@length 1bar\n");
    CHECK (readTextFile (saved.file ("notes to self.txt")) == "kick on 1");
}

TEST_CASE ("a file that left the project in an earlier session is still moved: the folder is opened with it in the project")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (RemoveTrack { "trk-bass" }, claude).ok);
    const auto result = saved.bus->save ({}, lee);

    REQUIRE (result.ok);
    CHECK (result.moved == std::vector<MovedFile> { { "clips/bass.notes", "unused/clips/bass.notes" } });

    // A new session doesn't have it in the project, so never moves it again,
    // even back in clips/ by hand.
    saved.open();
    std::filesystem::copy_file (saved.file ("unused/clips/bass.notes"), saved.file ("clips/bass.notes"));
    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    const auto later = saved.bus->save ({}, lee);

    REQUIRE (later.ok);
    CHECK (later.moved.empty());
    CHECK (std::filesystem::exists (saved.file ("clips/bass.notes")));
}

TEST_CASE ("a clip file added and removed between saves was never on disk, so there's nothing to move")
{
    SavedProject saved;
    REQUIRE (saved.bus->execute (AddClip { "trk-bass", "5.1", ClipText { newBass }, "clp-verse" }, claude).ok);
    REQUIRE (saved.bus->execute (RemoveClip { "clp-verse" }, claude).ok);

    const auto result = saved.bus->save ({}, lee);

    REQUIRE (result.ok);
    CHECK (result.moved.empty());
    CHECK_FALSE (std::filesystem::exists (saved.file ("unused")));
}

TEST_CASE ("createProject makes an empty project in a new or empty folder, and nowhere else")
{
    const TemporaryFolder temporary (unicodeName);
    const auto emptyProject = project::write (project::Project {});

    SUBCASE ("a new folder, with folders above it made too")
    {
        const auto folder = temporary.path / pathFromUtf8 ("songs " + unicodeName + "/first");

        CHECK (storage::createProject (folder, lee) == "");
        CHECK (projectFilesIn (folder) == emptyProject);
        CHECK (loaded (folder) == project::Project {});
        CHECK (projectFilesIn (folder).at ("project.json")
               == "{\n  \"format\": \"hocket-project\",\n  \"formatVersion\": 0,\n  \"tempo\": 120,\n  \"timeSignature\": \"4/4\",\n  \"tracks\": []\n}\n");
    }

    SUBCASE ("an empty folder")
    {
        CHECK (storage::createProject (temporary.path, lee) == "");
        CHECK (projectFilesIn (temporary.path) == emptyProject);
    }

    SUBCASE ("a folder with something in it")
    {
        writeFile (temporary.path / pathFromUtf8 ("notes.txt"), "hello");

        const auto error = storage::createProject (temporary.path, lee);

        CHECK (contains (error, "' isn't empty. A new project goes only in a new or empty folder, so it never mixes with files already there"));
        CHECK (filesIn (temporary.path) == std::map<std::string, std::string> { { "notes.txt", "hello" } });
    }

    SUBCASE ("a project's folder")
    {
        REQUIRE (storage::createProject (temporary.path, lee) == "");

        const auto error = storage::createProject (temporary.path, lee);

        CHECK (contains (error, unicodeName + " " ));
        CHECK (contains (error, "' isn't empty: it holds a project already. "));
    }

    SUBCASE ("a file")
    {
        const auto file = temporary.path / pathFromUtf8 ("song");
        writeFile (file, "not a folder");

        CHECK (contains (storage::createProject (file, lee), "song' is a file; a new project needs a new or empty folder"));
        CHECK (readTextFile (file) == "not a folder");
    }
}
