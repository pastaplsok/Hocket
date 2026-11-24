// A save interrupted part-way (SPEC §4.5): stopped at any step, as a crash
// would stop it, the next open ends with the folder as the save meant, or, if
// it stopped before it began putting files in place, as it was before. And
// finishing it, stopped at any step, is finished by the open after.

#include "StorageTesting.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    using Step = storage::ProjectFolder::SaveStep;

    const std::string newBass = "@length 4bars\n\n1.1 C2 1/2 v100\n";
    const std::string newVerse = "@length 1bar\n\n1.1 G2 1/4 v90\n";
    const std::string byHand = "@length 4bars\n\n1.1    C2 1/4 v100\n";

    // A save that writes every kind of file: a clip file changed, one added,
    // one leaving for unused/, project.json and .hocket/save.json.
    void makeChanges (CommandBus& bus)
    {
        REQUIRE (bus.execute (WriteClip { "clp-bass", newBass }, claude).ok);
        REQUIRE (bus.execute (AddClip { "trk-bass", "5.1", ClipText { newVerse }, "clp-verse" }, claude).ok);
        REQUIRE (bus.execute (RemoveClip { "clp-fill" }, claude).ok);
        REQUIRE (bus.execute (SetTempo { 100 }, claude).ok);
    }

    // The folder's files, but not the kept saves: a save stopped before it
    // began putting files in place may already have kept the version it
    // replaces, which is a whole and true copy.
    std::map<std::string, std::string> versionOf (const std::filesystem::path& folder)
    {
        auto files = filesIn (folder);
        std::erase_if (files, [] (const auto& file) { return file.first.starts_with (".hocket/saves/"); });
        return files;
    }

    // What a save stopped part-way leaves, and finishing it clears.
    std::vector<std::string> leftoversIn (const std::filesystem::path& folder)
    {
        std::vector<std::string> leftovers;

        for (const auto& entry : std::filesystem::recursive_directory_iterator (folder))
            if (const auto name = relativeName (entry.path(), folder);
                name.ends_with (storage::temporarySuffix) || name == storage::savingMarkerFile || name.starts_with (".hocket/saves/saving "))
                leftovers.push_back (name);

        return leftovers;
    }

    const char* kindName (Step::Kind kind)
    {
        switch (kind)
        {
            case Step::Kind::writeTemporary: return "writeTemporary";
            case Step::Kind::keepFile:       return "keepFile";
            case Step::Kind::nameKept:       return "nameKept";
            case Step::Kind::readBack:       return "readBack";
            case Step::Kind::writeMarker:    return "writeMarker";
            case Step::Kind::swap:           return "swap";
            case Step::Kind::moveUnused:     return "moveUnused";
            case Step::Kind::removeMarker:   return "removeMarker";
            case Step::Kind::clearLeftover:  return "clearLeftover";
        }

        return "?";
    }

    // A hook that lets `stop` steps happen, then stops at the next, as a crash would.
    std::function<bool (const Step&)> stopAt (std::size_t stop)
    {
        return [stop, taken = std::size_t { 0 }] (const Step&) mutable { return taken++ < stop; };
    }

    std::size_t firstOf (const std::vector<Step>& steps, Step::Kind kind)
    {
        const auto found = std::ranges::find (steps, kind, &Step::kind);
        REQUIRE (found != steps.end());
        return static_cast<std::size_t> (found - steps.begin());
    }

    // The next open, as --play does it: finish what a save left, then load.
    project::Project openedAgain (const std::filesystem::path& folder)
    {
        storage::finishInterruptedSave (folder);
        auto [result, opened] = storage::ProjectFolder::open (folder);

        for (const auto& error : result.errors)
            MESSAGE (project::formatDiagnostic (error));

        REQUIRE (result.project);
        return std::move (*result.project);
    }

    // The steps a whole save takes, and the folder before and after it.
    struct WholeSave
    {
        std::vector<Step> steps;
        std::map<std::string, std::string> before, after;
        project::Project beforeProject, afterProject;

        WholeSave()
        {
            SavedProject saved;
            beforeProject = saved.bus->project();
            makeChanges (*saved.bus);
            before = versionOf (saved.folder);
            saved.projectFolder->setStepHook ([this] (const Step& step) { steps.push_back (step); return true; });
            REQUIRE (saved.bus->save ({}, claude).ok);
            after = versionOf (saved.folder);
            afterProject = saved.bus->project();
        }
    };
}

TEST_CASE ("a save stopped at any step: the next open ends with the folder as the save meant, or as it was if the save hadn't begun putting files in place")
{
    const WholeSave whole;
    const auto marker = firstOf (whole.steps, Step::Kind::writeMarker);
    REQUIRE (whole.before != whole.after);
    REQUIRE (whole.after.contains ("unused/clips/fill.notes"));

    for (std::size_t stop = 0; stop < whole.steps.size(); ++stop)
    {
        CAPTURE (stop);
        CAPTURE (kindName (whole.steps[stop].kind));
        CAPTURE (utf8FromPath (whole.steps[stop].path.filename()));

        SavedProject saved;
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook (stopAt (stop));
        CHECK_FALSE (saved.bus->save ({}, claude).ok);

        // Past the marker, the save is under way; --check says so, changing nothing.
        const auto begun = stop > marker;
        CHECK (storage::hasInterruptedSave (saved.folder) == begun);

        const auto project = openedAgain (saved.folder);

        CHECK (versionOf (saved.folder) == (begun ? whole.after : whole.before));
        CHECK (project == (begun ? whole.afterProject : whole.beforeProject));
        CHECK (leftoversIn (saved.folder).empty());
        CHECK_FALSE (storage::hasInterruptedSave (saved.folder));
    }
}

TEST_CASE ("finishing an interrupted save, stopped at any step, is finished by the open after")
{
    // A save stopped as it began putting files in place.
    const auto interrupted = [] (SavedProject& saved)
    {
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook ([] (const Step& step) { return step.kind != Step::Kind::swap; });
        REQUIRE_FALSE (saved.bus->save ({}, claude).ok);
        REQUIRE (storage::hasInterruptedSave (saved.folder));
    };

    const WholeSave whole;
    std::vector<Step> steps;

    {
        SavedProject saved;
        interrupted (saved);
        const auto finished = storage::finishInterruptedSave (saved.folder, [&steps] (const Step& step) { steps.push_back (step); return true; });

        CHECK (finished.interrupted);
        CHECK (finished.message
               == "Finished a save by Claude, begun " + localTimeText (std::chrono::floor<std::chrono::seconds> (saved.now))
                      + ", which was interrupted part-way: wrote clips/bass.notes, clips/verse.notes and project.json; moved clips/fill.notes to "
                        "unused/clips/fill.notes, since no clip uses it any more.");
        CHECK (versionOf (saved.folder) == whole.after);
    }

    REQUIRE (steps.size() >= 6); // four files put in place, one moved, the marker removed

    for (std::size_t stop = 0; stop < steps.size(); ++stop)
    {
        CAPTURE (stop);
        CAPTURE (kindName (steps[stop].kind));

        SavedProject saved;
        interrupted (saved);

        CHECK (storage::finishInterruptedSave (saved.folder, stopAt (stop)).message == "stopped part-way by the step hook");
        CHECK (storage::hasInterruptedSave (saved.folder));

        CHECK (openedAgain (saved.folder) == whole.afterProject);
        CHECK (versionOf (saved.folder) == whole.after);
        CHECK (leftoversIn (saved.folder).empty());
    }
}

TEST_CASE ("finishing an interrupted save leaves a file changed since as it is, and puts the save's version of it in unused/")
{
    SavedProject saved;
    makeChanges (*saved.bus);
    saved.projectFolder->setStepHook ([] (const Step& step) { return step.kind != Step::Kind::swap; });
    REQUIRE_FALSE (saved.bus->save ({}, claude).ok);
    const auto savesBass = readTextFile (saved.file ("clips/bass.notes" + std::string (storage::temporarySuffix)));
    REQUIRE (savesBass);

    writeFile (saved.file ("clips/bass.notes"), byHand); // after the crash, before the next open

    const auto finished = storage::finishInterruptedSave (saved.folder);

    CHECK (finished.message
           == "Finished a save by Claude, begun " + localTimeText (std::chrono::floor<std::chrono::seconds> (saved.now))
                  + ", which was interrupted part-way: wrote clips/verse.notes and project.json; moved clips/fill.notes to unused/clips/fill.notes, "
                    "since no clip uses it any more; left clips/bass.notes as it is, since it changed after the save stopped, and put the save's "
                    "version of it in unused/clips/bass.notes.");
    CHECK (readTextFile (saved.file ("clips/bass.notes")) == byHand);
    CHECK (readTextFile (saved.file ("unused/clips/bass.notes")) == savesBass);
    CHECK (leftoversIn (saved.folder).empty());
    CHECK_FALSE (storage::hasInterruptedSave (saved.folder));
}

TEST_CASE ("a marker cut short as it was written is a save that changed nothing")
{
    SavedProject saved;
    const auto before = versionOf (saved.folder);
    writeFile (saved.file (std::string (storage::savingMarkerFile)), "{ \"format\": \"hocket-saving\", \"files\": [");
    writeFile (saved.file ("clips/bass.notes" + std::string (storage::temporarySuffix)), newBass);

    CHECK_FALSE (storage::hasInterruptedSave (saved.folder));
    const auto finished = storage::finishInterruptedSave (saved.folder);

    CHECK_FALSE (finished.interrupted);
    CHECK (finished.message == "Cleared the temporary files a save left when it was interrupted before it changed anything.");
    CHECK (versionOf (saved.folder) == before);
    CHECK (leftoversIn (saved.folder).empty());
}

//==============================================================================
// The PR review's findings 1 to 3 (PROGRESS.md, 2026-11-24): each fails as the
// code is, which should_fail records, until its fix takes the mark off.
namespace
{
    // A save stopped as it began putting files in place, as a crash would.
    void interruptedAtFirstSwap (SavedProject& saved)
    {
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook ([] (const Step& step) { return step.kind != Step::Kind::swap; });
        REQUIRE_FALSE (saved.bus->save ({}, claude).ok);
        REQUIRE (storage::hasInterruptedSave (saved.folder));
    }

    // Every file under the folder holding exactly this text.
    std::vector<std::string> filesHolding (const std::filesystem::path& folder, const std::string& text)
    {
        std::vector<std::string> found;

        for (const auto& [name, bytes] : filesIn (folder))
            if (bytes == text)
                found.push_back (name);

        return found;
    }

    std::string temporaryOf (const std::string& file)
    {
        return file + std::string (storage::temporarySuffix);
    }
}

TEST_CASE ("review 1: finishing that fails leaves the save to finish, and a session's save never deletes what's left of it" * doctest::should_fail())
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    const auto savesBass = readTextFile (saved.file (temporaryOf ("clips/bass.notes")));
    REQUIRE (savesBass);

    {
        // Another program holds clips/bass.notes as --play starts, so it can't be put in place.
        const auto held = platform::FileLock::acquire (saved.file ("clips/bass.notes"));
        REQUIRE (held);
        storage::finishInterruptedSave (saved.folder);
    }

    // --play went on to open the folder, and the person saved a change.
    saved.open();
    REQUIRE (saved.bus->execute (SetTempo { 120 }, lee).ok);
    saved.bus->save ({}, lee);

    // The next --play.
    storage::finishInterruptedSave (saved.folder);
    CHECK_FALSE (filesHolding (saved.folder, *savesBass).empty());
}

TEST_CASE ("review 2: a marker that can't be read deletes nothing: the save's new texts stay, and the folder still loads" * doctest::should_fail())
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    const auto savesBass = *readTextFile (saved.file (temporaryOf ("clips/bass.notes")));

    {
        // Something holds the marker as --play starts (a backup or sync tool, say).
        const auto held = platform::FileLock::acquire (saved.file (std::string (storage::savingMarkerFile)));
        REQUIRE (held);
        storage::finishInterruptedSave (saved.folder);
    }

    storage::finishInterruptedSave (saved.folder);
    CHECK_FALSE (filesHolding (saved.folder, savesBass).empty());
    CHECK (project::load (saved.folder).project);
}

TEST_CASE ("review 3: finishing never moves a clip file out from under a project.json changed since the save stopped" * doctest::should_fail())
{
    SavedProject saved;
    const auto oldProjectJson = *readTextFile (saved.file ("project.json"));
    interruptedAtFirstSwap (saved);

    // After the crash, before the next --play: project.json written again by
    // hand (a checkout, say), still naming clips/fill.notes.
    writeFile (saved.file ("project.json"), oldProjectJson + "\n");

    storage::finishInterruptedSave (saved.folder);
    CHECK (project::load (saved.folder).project);
}

TEST_CASE ("a marker never moves a file that isn't one of the project's")
{
    SavedProject saved;
    const auto before = versionOf (saved.folder);
    writeFile (saved.folder.parent_path() / pathFromUtf8 ("elsewhere.txt"), "not the project's");
    writeFile (saved.file ("../elsewhere.txt" + std::string (storage::temporarySuffix)), "a damaged marker's");
    writeFile (saved.file (std::string (storage::savingMarkerFile)),
               "{ \"format\": \"hocket-saving\", \"author\": \"Lee\", \"time\": \"2026-11-20T09:15:00Z\", \"files\": [ { \"file\": \"../elsewhere.txt\", "
               "\"before\": { \"exists\": true, \"modified\": 0, \"size\": 0 } } ], \"unused\": [] }");

    CHECK_FALSE (storage::hasInterruptedSave (saved.folder));
    storage::finishInterruptedSave (saved.folder);

    CHECK (readTextFile (saved.folder.parent_path() / pathFromUtf8 ("elsewhere.txt")) == "not the project's");
    CHECK (versionOf (saved.folder) == before);
}
