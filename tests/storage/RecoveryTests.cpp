// The recovery copy (SPEC Â§4.5): written outside the project's folder a few
// seconds after changes stop, so a crash loses nothing; whole even when a crash
// or power cut comes part-way through writing it (Lee, 2026-11-21); removed
// once nothing is unsaved; and, when a session left one, played by --recover
// or filed with the kept saves by --discard-recovery, while plain --play stops
// and asks.

#include "StorageTesting.h"

#include "storage/Formats.h"
#include "storage/Recovery.h"
#include "storage/RecoveryCopy.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;
using namespace std::chrono_literals;

namespace
{
    using Step = storage::RecoveryCopy::Step;
    using Clock = storage::RecoveryKeeper::Clock;

    const std::string newBass = "@length 4bars\n\n1.1 C2 1/2 v100\n";
    const std::string newVerse = "@length 2bars\n\n1.1 E2 1/4 v90\n";

    // When the tests' recovery copies are written: after the project's files
    // were, which is now, so a copy is newer than its files.
    const auto written = std::chrono::system_clock::time_point (std::chrono::floor<std::chrono::seconds> (std::chrono::system_clock::now()) + 1h);

    // A copy's project files: all but its own .hocket/recovery.json.
    std::map<std::string, std::string> projectFilesOf (const std::filesystem::path& copy)
    {
        auto files = filesIn (copy);
        files.erase (std::string (storage::recoveryInfoFile));
        return files;
    }

    // What's in a folder, by name.
    std::vector<std::string> namesIn (const std::filesystem::path& folder)
    {
        std::vector<std::string> names;

        for (const auto& entry : std::filesystem::directory_iterator (folder))
            names.push_back (utf8FromPath (entry.path().filename()));

        std::ranges::sort (names);
        return names;
    }

    // A saved project, as --play opens it, and a recovery folder of its own.
    struct RecoveryScene
    {
        SavedProject saved;
        TemporaryFolder recoveryFolder { "recovery " + unicodeName };

        storage::RecoveryCopy copy() const
        {
            return storage::RecoveryCopy (recoveryFolder.path, saved.folder, [] { return written; });
        }

        std::map<std::string, std::string> files() const { return project::write (saved.bus->project()); }
    };

    // The step a crash stops at, and what it leaves there: a file it was
    // writing half-written, a folder it was deleting part-deleted.
    struct Crash
    {
        std::size_t at = 0;
        std::map<std::string, std::string> writing; // the files the write was writing
        std::size_t steps = 0;

        bool operator() (const Step& step)
        {
            if (steps++ < at)
                return true;

            if (step.kind == Step::Kind::writeFile)
            {
                // Half its bytes, as a write cut off part-way leaves it.
                auto relative = utf8FromPath (step.path.lexically_relative (step.path.parent_path()));

                for (auto folder = step.path.parent_path(); ! utf8FromPath (folder.filename()).starts_with ("writing "); folder = folder.parent_path())
                    relative = utf8FromPath (folder.filename()) + "/" + relative;

                const auto found = writing.find (relative);
                const auto text = found != writing.end() ? found->second : std::string ("{\n  \"format\": \"hocket-recovery\",\n  \"project\": \"D:\\\\Mu");
                std::filesystem::create_directories (step.path.parent_path());
                REQUIRE (platform::writeFileToDisk (step.path, text.substr (0, text.size() / 2)).empty());
            }
            else if (step.kind == Step::Kind::deleteTakenAway)
            {
                std::filesystem::remove (step.path / "project.json");
            }

            return false;
        }
    };
}

TEST_CASE ("a recovery copy is a project folder of its own: it loads as the session's project, and names the project and when it was written")
{
    RecoveryScene scene;
    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    REQUIRE (scene.saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);

    auto copy = scene.copy();
    CHECK_FALSE (copy.find());
    REQUIRE (copy.write (scene.files()).empty());

    const auto found = copy.find();
    REQUIRE (found);
    CHECK (found->folder == copy.place() / "1");
    CHECK (found->written == std::chrono::floor<std::chrono::seconds> (written));
    CHECK (projectFilesOf (found->folder) == scene.files());
    CHECK (loaded (found->folder) == scene.saved.bus->project());

    // Its place is named for the project's folder, then a hash of its path.
    const auto place = utf8FromPath (copy.place().filename());
    CHECK (place.starts_with ("song " + unicodeName + " "));
    CHECK (place.size() == ("song " + unicodeName + " ").size() + 16);

    const auto info = *readTextFile (found->folder / pathFromUtf8 (storage::recoveryInfoFile));
    CHECK (contains (info, "\"format\": \"hocket-recovery\""));
    CHECK (contains (info, "\"written\": \"" + storage::utcTimeText (std::chrono::floor<std::chrono::seconds> (written)) + "\""));
    CHECK (copy.project() == utf8FromPath (std::filesystem::canonical (scene.saved.folder)));

    // A newer copy takes its place, and the one before goes.
    REQUIRE (scene.saved.bus->execute (SetTempo { 110 }, lee).ok);
    REQUIRE (copy.write (scene.files()).empty());
    CHECK (namesIn (copy.place()) == std::vector<std::string> { "2" });
    CHECK (projectFilesOf (copy.find()->folder) == scene.files());

    REQUIRE (copy.remove().empty());
    CHECK_FALSE (copy.find());
    CHECK_FALSE (std::filesystem::exists (copy.place()));
}

TEST_CASE ("a project folder has one place for its recovery copy, however its path is written; another folder has another")
{
    RecoveryScene scene;
    const auto& folder = scene.saved.folder;
    const auto place = scene.copy().place();

    CHECK (storage::RecoveryCopy (scene.recoveryFolder.path, pathFromUtf8 (utf8FromPath (folder) + "/")).place() == place);
    CHECK (storage::RecoveryCopy (scene.recoveryFolder.path, folder / "clips" / "..").place() == place);
    CHECK (storage::RecoveryCopy (scene.recoveryFolder.path, folder / ".").place() == place);

#ifdef _WIN32
    // Windows' names don't care about case.
    auto upper = utf8FromPath (folder);
    std::ranges::transform (upper, upper.begin(), [] (char c) { return c >= 'a' && c <= 'z' ? static_cast<char> (c - 'a' + 'A') : c; });
    CHECK (storage::RecoveryCopy (scene.recoveryFolder.path, pathFromUtf8 (upper)).place() == place);
#endif

    const auto other = scene.saved.temporary.path / pathFromUtf8 ("other " + unicodeName);
    std::filesystem::create_directories (other);
    CHECK (storage::RecoveryCopy (scene.recoveryFolder.path, other).place() != place);

    // A long name is cut short, so the copy's files' paths stay short.
    const auto longName = scene.saved.temporary.path / pathFromUtf8 (std::string (80, 'x'));
    std::filesystem::create_directories (longName);
    const auto longPlace = utf8FromPath (storage::RecoveryCopy (scene.recoveryFolder.path, longName).place().filename());
    CHECK (longPlace.starts_with (std::string (40, 'x') + " "));
    CHECK (longPlace.size() == 40 + 1 + 16);
}

TEST_CASE ("a crash or power loss part-way through writing a recovery copy leaves the copy before it whole and usable, never a half-written one")
{
    // A power cut can't be made in a test. What gets a copy through one is
    // the order checked here: every file of the new copy is on the disk
    // (writeFileToDisk flushes it) before the rename that makes the copy
    // whole, which is written through, and the copy before is taken away
    // only after it. So a crash at any step, which this test makes at each in
    // turn, shows what a power cut there would leave.
    RecoveryScene scene;

    // The copy before, and a new one that changes project.json, rewrites a
    // clip file, adds one and drops another.
    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    const auto before = scene.files();

    REQUIRE (scene.saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (scene.saved.bus->execute (AddClip { "trk-bass", "5.1", ClipText { newVerse }, "clp-verse" }, claude).ok);
    REQUIRE (scene.saved.bus->execute (RemoveClip { "clp-fill" }, claude).ok);
    const auto after = scene.files();
    REQUIRE (after.contains ("clips/verse.notes"));
    REQUIRE_FALSE (after.contains ("clips/fill.notes"));

    const auto startWithCopyOf = [&scene] (const std::map<std::string, std::string>& files)
    {
        std::filesystem::remove_all (scene.recoveryFolder.path);
        std::filesystem::create_directories (scene.recoveryFolder.path);
        REQUIRE (scene.copy().write (files).empty());
    };

    // Every step a write takes when nothing stops it, in order.
    startWithCopyOf (before);
    auto recording = scene.copy();
    std::vector<Step> steps;
    recording.setStepHook ([&steps] (const Step& step) { steps.push_back (step); return true; });
    REQUIRE (recording.write (after).empty());

    const auto place = recording.place();
    const auto commit = static_cast<std::size_t> (std::ranges::find (steps, Step::Kind::commit, &Step::kind) - steps.begin());
    REQUIRE (commit == after.size() + 1); // each file, then recovery.json, then the rename
    REQUIRE (steps.size() == commit + 3);

    for (std::size_t i = 0; i < commit; ++i)
    {
        CHECK (steps[i].kind == Step::Kind::writeFile);
        CHECK (steps[i].path.lexically_relative (place).begin()->filename() == "writing 2");
    }

    CHECK (utf8FromPath (steps[commit - 1].path).ends_with ("recovery.json")); // which project, and when: last
    CHECK (steps[commit].path == place / "2");
    CHECK ((steps[commit + 1].kind == Step::Kind::takeAway && steps[commit + 1].path == place / "1"));
    CHECK ((steps[commit + 2].kind == Step::Kind::deleteTakenAway && steps[commit + 2].path == place / "removing 1"));

    // A crash at each step in turn.
    for (std::size_t at = 0; at < steps.size(); ++at)
    {
        CAPTURE (at);
        startWithCopyOf (before);

        auto crashing = scene.copy();
        crashing.setStepHook (Crash { at, after });
        CHECK_FALSE (crashing.write (after).empty());

        // The next session finds a whole copy: the one before, until the
        // rename makes the new one whole; the new one after it.
        const auto found = scene.copy().find();
        REQUIRE (found);
        const auto read = project::load (found->folder);

        for (const auto& error : read.errors)
            MESSAGE (project::formatDiagnostic (error));

        CHECK (read.errors.empty());
        CHECK (projectFilesOf (found->folder) == (at <= commit ? before : after));

        // And the next write clears away what the crash left.
        REQUIRE (scene.copy().write (after).empty());
        const auto names = namesIn (place);
        REQUIRE (names.size() == 1);
        CHECK (names.front().find_first_not_of ("0123456789") == std::string::npos);
        CHECK (projectFilesOf (scene.copy().find()->folder) == after);
    }

    // With no copy before, a crash before the rename leaves none, rather than half of one.
    for (std::size_t at = 0; at <= commit; ++at)
    {
        CAPTURE (at);
        std::filesystem::remove_all (scene.recoveryFolder.path);

        auto crashing = scene.copy();
        crashing.setStepHook (Crash { at, after });
        CHECK_FALSE (crashing.write (after).empty());
        CHECK_FALSE (scene.copy().find());
    }
}

TEST_CASE ("a crash part-way through removing a recovery copy leaves it whole or gone, never half there")
{
    RecoveryScene scene;
    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    const auto files = scene.files();

    for (std::size_t at = 0; at < 2; ++at) // taking it away, then deleting it
    {
        CAPTURE (at);
        std::filesystem::remove_all (scene.recoveryFolder.path);
        REQUIRE (scene.copy().write (files).empty());

        auto crashing = scene.copy();
        crashing.setStepHook (Crash { at, files });
        CHECK_FALSE (crashing.remove().empty());

        const auto found = scene.copy().find();
        CHECK (found.has_value() == (at == 0));

        if (found)
            CHECK (projectFilesOf (found->folder) == files);

        // The next remove clears it all, the place included.
        REQUIRE (scene.copy().remove().empty());
        CHECK_FALSE (std::filesystem::exists (scene.copy().place()));
    }
}

TEST_CASE ("a recovery copy that can't be written leaves the copy before it")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    const auto before = scene.files();
    REQUIRE (copy.write (before).empty());

    // A file named like the folder another file is in can't be written.
    auto broken = before;
    broken["clips"] = "a file where a folder should be";
    const auto problem = copy.write (broken);

    CHECK (contains (problem, "couldn't write the recovery copy in '" + utf8FromPath (copy.place()) + "': couldn't write its "));
    CHECK (namesIn (copy.place()) == std::vector<std::string> { "1" });
    CHECK (projectFilesOf (copy.find()->folder) == before);
}

TEST_CASE ("a recovery copy of another project, where this one's would go, is never read as this one's or written over")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    writeFile (copy.place() / "3" / "project.json", "{}");
    writeFile (copy.place() / "3" / pathFromUtf8 (storage::recoveryInfoFile),
               "{ \"format\": \"hocket-recovery\", \"project\": \"D:\\\\elsewhere\", \"written\": \"2026-11-21T10:00:00Z\" }");
    const auto before = filesIn (copy.place());

    CHECK_FALSE (copy.find());
    CHECK (contains (copy.write (scene.files()), "holds a copy of another project, D:\\elsewhere, which Hocket won't write over"));
    CHECK (copy.remove().empty());
    CHECK (filesIn (copy.place()) == before);
}

//==============================================================================
TEST_CASE ("the keeper writes the recovery copy once changes have stopped for 3 s, and again only when the project changes")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    storage::RecoveryKeeper keeper (*scene.saved.bus, *scene.saved.projectFolder, copy, false);
    const auto start = Clock::time_point {} + 1h;

    CHECK_FALSE (keeper.poll (start));
    CHECK_FALSE (copy.find()); // nothing unsaved

    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    CHECK_FALSE (keeper.poll (start + 1s)); // the change, seen
    REQUIRE (scene.saved.bus->execute (SetTempo { 101 }, lee).ok);
    CHECK_FALSE (keeper.poll (start + 2s)); // another: it waits 3 s from this one
    CHECK_FALSE (keeper.poll (start + 4900ms));
    CHECK_FALSE (copy.find());

    CHECK_FALSE (keeper.poll (start + 5s));
    REQUIRE (copy.find());
    CHECK (projectFilesOf (copy.find()->folder) == scene.files());
    CHECK (copy.find()->folder.filename() == "1");

    // Nothing new, nothing written.
    CHECK_FALSE (keeper.poll (start + 20s));
    CHECK (copy.find()->folder.filename() == "1");

    REQUIRE (scene.saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    CHECK_FALSE (keeper.poll (start + 21s));
    CHECK_FALSE (keeper.poll (start + 24s));
    CHECK (copy.find()->folder.filename() == "2");
    CHECK (projectFilesOf (copy.find()->folder) == scene.files());
}

TEST_CASE ("changes that keep coming are copied at least every 30 s")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    storage::RecoveryKeeper keeper (*scene.saved.bus, *scene.saved.projectFolder, copy, false);
    const auto start = Clock::time_point {} + 1h;
    std::optional<Clock::duration> firstCopy;

    for (int i = 0; i < 80 && ! firstCopy; ++i)
    {
        REQUIRE (scene.saved.bus->execute (SetTempo { 100.0 + i }, lee).ok);
        keeper.poll (start + i * 1s);

        if (copy.find())
            firstCopy = i * 1s;
    }

    REQUIRE (firstCopy);
    CHECK (*firstCopy == 30s);
}

TEST_CASE ("the copy goes once nothing is unsaved: after a save, or undo back to it; redo brings it back")
{
    RecoveryScene scene;
    auto& bus = *scene.saved.bus;
    auto copy = scene.copy();
    storage::RecoveryKeeper keeper (bus, *scene.saved.projectFolder, copy, false);
    auto now = Clock::time_point {} + 1h;

    const auto settle = [&]
    {
        keeper.poll (now);
        now += 4s;
        keeper.poll (now);
    };

    REQUIRE (bus.execute (SetTempo { 100 }, lee).ok);
    settle();
    REQUIRE (copy.find());

    // Undo back to the saved state: gone at the next poll, without waiting.
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    keeper.poll (now += 50ms);
    CHECK_FALSE (copy.find());

    REQUIRE (bus.redo (lee).ok);
    settle();
    REQUIRE (copy.find());

    REQUIRE (bus.save ({}, lee).ok);
    keeper.poll (now += 50ms);
    CHECK_FALSE (copy.find());
    CHECK_FALSE (std::filesystem::exists (copy.place()));
}

TEST_CASE ("a change put back by hand leaves nothing to keep, though the history counts it as unsaved")
{
    RecoveryScene scene;
    auto& bus = *scene.saved.bus;
    auto copy = scene.copy();
    storage::RecoveryKeeper keeper (bus, *scene.saved.projectFolder, copy, false);
    const auto start = Clock::time_point {} + 1h;

    REQUIRE (bus.execute (SetTempo { 100 }, lee).ok);
    keeper.poll (start);
    keeper.poll (start + 4s);
    REQUIRE (copy.find());

    REQUIRE (bus.execute (SetTempo { 174 }, lee).ok); // as it's saved
    REQUIRE (bus.hasUnsavedChanges());
    keeper.poll (start + 5s);
    keeper.poll (start + 9s);
    CHECK_FALSE (copy.find());
}

TEST_CASE ("at a clean exit, the copy goes if nothing is unsaved, and otherwise is brought up to date, and --play says where it is")
{
    RecoveryScene scene;
    auto& bus = *scene.saved.bus;
    auto copy = scene.copy();

    {
        storage::RecoveryKeeper keeper (bus, *scene.saved.projectFolder, copy, false);
        CHECK (keeper.finish().empty());
        CHECK_FALSE (copy.find());
    }

    // A change made a moment before the exit, so not yet in the copy.
    {
        storage::RecoveryKeeper keeper (bus, *scene.saved.projectFolder, copy, false);
        REQUIRE (bus.execute (SetTempo { 100 }, lee).ok);
        keeper.poll (Clock::time_point {} + 1h);
        keeper.poll (Clock::time_point {} + 1h + 4s);
        REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);

        const auto said = keeper.finish();
        REQUIRE (copy.find());
        CHECK (projectFilesOf (copy.find()->folder) == scene.files());
        CHECK (said == "Unsaved changes are kept in a recovery copy, '" + utf8FromPath (copy.find()->folder)
                           + "'. --play --recover plays them next time; --play --discard-recovery files them with the project's kept saves.");
    }

    // Saved, then a clean exit: gone.
    {
        storage::RecoveryKeeper keeper (bus, *scene.saved.projectFolder, copy, true);
        REQUIRE (bus.save ({}, lee).ok);
        CHECK (keeper.finish().empty());
        CHECK_FALSE (copy.find());
    }
}

TEST_CASE ("a recovery copy that can't be written is said once, tried again every 10 s, and said again once it's written")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    storage::RecoveryKeeper keeper (*scene.saved.bus, *scene.saved.projectFolder, copy, false);
    const auto start = Clock::time_point {} + 1h;

    // A file where its place's folder should be.
    writeFile (copy.place(), "in the way");

    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    CHECK_FALSE (keeper.poll (start));

    const auto said = keeper.poll (start + 3s);
    REQUIRE (said);
    CHECK (said->starts_with ("Couldn't write the recovery copy, so a crash now would lose the changes made since the last save: couldn't make the folder '"));
    CHECK (said->ends_with (". Trying again every 10 s."));

    CHECK_FALSE (keeper.poll (start + 8s));  // waiting
    CHECK_FALSE (keeper.poll (start + 13s)); // tried again, and failed the same way: not said again
    CHECK_FALSE (copy.find());

    std::filesystem::remove (copy.place());
    CHECK_FALSE (keeper.poll (start + 18s));
    CHECK (keeper.poll (start + 23s) == "The recovery copy is written again.");
    CHECK (copy.find());
}

//==============================================================================
TEST_CASE ("with no recovery copy, --play plays; --recover has nothing to play, and --discard-recovery nothing to discard")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    const auto& project = scene.saved.bus->project();

    const auto none = storage::startRecovery (copy, *scene.saved.projectFolder, project, storage::RecoveryChoice::none);
    CHECK (none.proceed);
    CHECK (none.message.empty());

    const auto recover = storage::startRecovery (copy, *scene.saved.projectFolder, project, storage::RecoveryChoice::recover);
    CHECK_FALSE (recover.proceed);
    CHECK (recover.message.starts_with ("--recover: there's no recovery copy of this project, so there's nothing to recover"));

    const auto discard = storage::startRecovery (copy, *scene.saved.projectFolder, project, storage::RecoveryChoice::discard);
    CHECK (discard.proceed);
    CHECK (discard.message == "--discard-recovery: there's no recovery copy of this project to discard.");
}

TEST_CASE ("a recovery copy that holds nothing the files don't is removed, and --play plays")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    REQUIRE (copy.write (scene.files()).empty()); // as a crash between a save and its removal leaves it

    const auto start = storage::startRecovery (copy, *scene.saved.projectFolder, scene.saved.bus->project(), storage::RecoveryChoice::none);
    CHECK (start.proceed);
    CHECK (start.message == "Found a recovery copy written " + localTimeText (std::chrono::floor<std::chrono::seconds> (written))
                                + " that holds nothing the project's files don't, so it's removed.");
    CHECK_FALSE (copy.find());
}

TEST_CASE ("a crash loses nothing: plain --play stops and asks, and --recover plays the changes as one entry, unsaved, which undo takes back")
{
    RecoveryScene scene;
    const auto saved = scene.saved.bus->project();

    // The session before: changes, a copy written once they stopped, then a crash.
    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    REQUIRE (scene.saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    REQUIRE (scene.saved.bus->execute (AddClip { "trk-bass", "5.1", ClipText { newVerse }, "clp-verse" }, claude).ok);
    const auto lost = scene.saved.bus->project();

    {
        auto copy = scene.copy();
        storage::RecoveryKeeper keeper (*scene.saved.bus, *scene.saved.projectFolder, copy, false);
        keeper.poll (Clock::time_point {} + 1h);
        keeper.poll (Clock::time_point {} + 1h + 4s);
        REQUIRE (copy.find());
    }

    const auto before = filesIn (scene.saved.folder);
    scene.saved.open();
    REQUIRE (scene.saved.bus->project() == saved);
    auto copy = scene.copy();
    const auto copyFolder = copy.find()->folder;
    const auto time = localTimeText (std::chrono::floor<std::chrono::seconds> (written));

    // Plain --play: nothing played, nothing changed, and what to do.
    const auto asked = storage::startRecovery (copy, *scene.saved.projectFolder, saved, storage::RecoveryChoice::none);
    CHECK_FALSE (asked.proceed);
    CHECK (asked.message
           == "This project has a recovery copy, written " + time
                  + ", holding changes that were never saved: the session that made them ended without saving them, by a crash or by quitting.\n"
                    "It's newer than the project's files.\n"
                    "Changes: the tempo (174 BPM to 100 BPM); changed trk-bass; rewrote the notes of 1 clip file.\n"
                    "Nothing was played. To go on, run --play again with one of these:\n"
                    "  --recover            play the recovery copy: its changes come in as one history entry, unsaved, which undo takes back\n"
                    "  --discard-recovery   play the files as saved, filing the recovery copy with the project's kept saves, so revert can still bring it back\n"
                    "The copy is in '" + utf8FromPath (copyFolder) + "'.");
    CHECK (copy.find()->folder == copyFolder);
    CHECK (filesIn (scene.saved.folder) == before);

    // --recover: the lost changes, as one entry.
    auto recover = storage::startRecovery (copy, *scene.saved.projectFolder, saved, storage::RecoveryChoice::recover);
    REQUIRE (recover.proceed);
    REQUIRE (recover.recovered);
    CHECK (recover.message.empty());
    CHECK (*recover.recovered == lost);
    CHECK (recover.description == "recovered the unsaved changes in the recovery copy written " + time);

    auto& bus = *scene.saved.bus;
    const auto result = bus.execute (ReplaceProject { std::move (*recover.recovered), recover.description }, lee);
    REQUIRE (result.ok);
    CHECK (bus.project() == lost);
    CHECK (bus.hasUnsavedChanges());
    REQUIRE (bus.history().size() == 1);
    CHECK (bus.history().front().description == recover.description);

    // The copy holds the session as it is now, so it isn't written again.
    storage::RecoveryKeeper keeper (bus, *scene.saved.projectFolder, copy, true);
    keeper.poll (Clock::time_point {} + 2h);
    keeper.poll (Clock::time_point {} + 2h + 4s);
    CHECK (copy.find()->folder == copyFolder);

    // Undo takes it back, to the files as saved, and then there's nothing to keep.
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project() == saved);
    keeper.poll (Clock::time_point {} + 2h + 5s);
    CHECK_FALSE (copy.find());
    CHECK (filesIn (scene.saved.folder) == before);
}

TEST_CASE ("a recovery copy older than the project's files says so")
{
    RecoveryScene scene;
    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    auto copy = scene.copy();
    REQUIRE (copy.write (scene.files()).empty());

    // The files changed after the copy was written: saved elsewhere, or edited by hand.
    const auto later = written + 1h;
    std::filesystem::last_write_time (scene.saved.file ("project.json"),
                                      std::chrono::clock_cast<std::filesystem::file_time_type::clock> (std::chrono::floor<std::chrono::seconds> (later)));
    scene.saved.open();

    const auto asked = storage::startRecovery (copy, *scene.saved.projectFolder, scene.saved.bus->project(), storage::RecoveryChoice::none);
    CHECK_FALSE (asked.proceed);
    CHECK (contains (asked.message, "\nIt's older than the project's files, which changed at " + localTimeText (std::chrono::floor<std::chrono::seconds> (later))
                                        + ", after it was written: they were saved or edited since. Recovering it would put its version in place of theirs.\n"));
}

TEST_CASE ("--discard-recovery files the recovery copy with the kept saves, labelled as a discarded recovery, and revert brings it back")
{
    RecoveryScene scene;
    const auto saved = scene.saved.bus->project();
    REQUIRE (scene.saved.bus->execute (SetTempo { 100 }, lee).ok);
    REQUIRE (scene.saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);
    const auto lost = scene.saved.bus->project();
    const auto lostFiles = scene.files();

    auto copy = scene.copy();
    REQUIRE (copy.write (lostFiles).empty());
    scene.saved.open();
    const auto projectFiles = projectFilesIn (scene.saved.folder);

    const auto start = storage::startRecovery (copy, *scene.saved.projectFolder, saved, storage::RecoveryChoice::discard);
    REQUIRE (start.proceed);
    CHECK_FALSE (start.recovered);

    // Kept like a save: numbered, timed by when the copy was written, and labelled.
    const auto kept = scene.saved.bus->keptSaves();
    REQUIRE (kept.size() == 1);
    const auto time = std::chrono::floor<std::chrono::seconds> (written);
    CHECK (kept.front().info.discardedRecovery);
    CHECK (kept.front().info.author.empty());
    CHECK (kept.front().info.time == time);
    CHECK (kept.front().folder == ".hocket/saves/1 " + [&] { auto text = localTimeText (time); std::ranges::replace (text, ':', '-'); return text; }()
                                      + " discarded recovery");
    CHECK (describe (kept.front()) == "save #1, a discarded recovery copy, " + localTimeText (time));
    CHECK (start.message == "Filed the recovery copy written " + localTimeText (time) + " as save #1, a discarded recovery copy, " + localTimeText (time)
                                + ", so revert can still bring it back. Playing the project's files as saved.");

    // Whole: the copy's files, and who it's by.
    auto keptFiles = filesIn (scene.saved.file (kept.front().folder));
    CHECK (contains (keptFiles.at (".hocket/save.json"), "\"discardedRecovery\": true"));
    keptFiles.erase (".hocket/save.json");
    CHECK (keptFiles == lostFiles);

    // Gone from the recovery folder; the project's files untouched; and a
    // second --play has nothing to ask.
    CHECK_FALSE (copy.find());
    CHECK (projectFilesIn (scene.saved.folder) == projectFiles);
    CHECK (storage::startRecovery (copy, *scene.saved.projectFolder, saved, storage::RecoveryChoice::none).message.empty());

    // Revert brings the changes back.
    REQUIRE (scene.saved.bus->execute (Revert { 1 }, lee).ok);
    CHECK (scene.saved.bus->project() == lost);
}

TEST_CASE ("a recovery copy that doesn't read can't be recovered, but can be filed with the kept saves as it is")
{
    RecoveryScene scene;
    auto copy = scene.copy();
    auto files = scene.files();
    files["clips/bass.notes"] = "@length 4bars\n\n1.1 not-a-note 1/4\n";
    REQUIRE (copy.write (files).empty());
    const auto copyFolder = copy.find()->folder;
    const auto& project = scene.saved.bus->project();

    const auto asked = storage::startRecovery (copy, *scene.saved.projectFolder, project, storage::RecoveryChoice::none);
    CHECK_FALSE (asked.proceed);
    CHECK (contains (asked.message, "\nIt doesn't read as a project, so it can't be played: "));
    CHECK_FALSE (contains (asked.message, "--recover "));
    CHECK (contains (asked.message, "--discard-recovery   play the files as saved"));

    const auto recover = storage::startRecovery (copy, *scene.saved.projectFolder, project, storage::RecoveryChoice::recover);
    CHECK_FALSE (recover.proceed);
    CHECK (recover.message.starts_with ("--recover: the recovery copy written " + localTimeText (std::chrono::floor<std::chrono::seconds> (written))
                                        + " doesn't read as a project, so it can't be played: "));
    CHECK (contains (recover.message, "\nIt's in '" + utf8FromPath (copyFolder) + "'. --discard-recovery files it with the project's kept saves"));

    const auto discard = storage::startRecovery (copy, *scene.saved.projectFolder, project, storage::RecoveryChoice::discard);
    REQUIRE (discard.proceed);
    const auto kept = scene.saved.bus->keptSaves();
    REQUIRE (kept.size() == 1);
    auto keptFiles = filesIn (scene.saved.file (kept.front().folder));
    keptFiles.erase (".hocket/save.json");
    CHECK (keptFiles == files);
    CHECK_FALSE (copy.find());
}
