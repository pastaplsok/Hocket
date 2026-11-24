// A save interrupted part-way (SPEC §4.5), and its two rules (Lee, 2026-11-24):
//
// 1. Finishing an interrupted save is all or nothing. It runs only when the
//    marker reads and parses, every file it still has to put in place is
//    there, and nothing the save replaces has changed since the crash.
//    Otherwise it changes nothing on disk, --play stops, and the message says
//    where the old version (the kept save) and the new one (the temporary
//    files) are. A step that fails part-way stops --play, and the next start
//    tries again. save() refuses while an older marker is there.
// 2. Nothing deletes a file whose text exists nowhere else; it's set aside in
//    unused/.
//
// Each step of a save, and of finishing one, is stopped (as a crash would
// stop it) and made to fail (as the OS might), and both rules are checked
// after every one.

#include "StorageTesting.h"

#include <fstream>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    using Step = storage::ProjectFolder::SaveStep;
    using StepOutcome = storage::ProjectFolder::StepOutcome;
    using Finished = storage::FinishedSave::Outcome;

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

    std::string temporaryOf (const std::string& file)
    {
        return file + std::string (storage::temporarySuffix);
    }

    // What a save stopped part-way leaves, outside unused/, where finishing
    // sets it aside.
    std::vector<std::string> leftoversIn (const std::filesystem::path& folder)
    {
        std::vector<std::string> leftovers;

        for (const auto& entry : std::filesystem::recursive_directory_iterator (folder))
            if (const auto name = relativeName (entry.path(), folder);
                ! name.starts_with ("unused/")
                && (name.ends_with (storage::temporarySuffix) || name == storage::savingMarkerFile || name.starts_with (".hocket/saves/saving ")))
                leftovers.push_back (name);

        return leftovers;
    }

    const char* kindName (Step::Kind kind)
    {
        switch (kind)
        {
            case Step::Kind::setAside:       return "setAside";
            case Step::Kind::writeTemporary: return "writeTemporary";
            case Step::Kind::keepFile:       return "keepFile";
            case Step::Kind::nameKept:       return "nameKept";
            case Step::Kind::readBack:       return "readBack";
            case Step::Kind::writeMarker:    return "writeMarker";
            case Step::Kind::nameMarker:     return "nameMarker";
            case Step::Kind::swap:           return "swap";
            case Step::Kind::putBack:        return "putBack";
            case Step::Kind::moveUnused:     return "moveUnused";
            case Step::Kind::removeMarker:   return "removeMarker";
        }

        return "?";
    }

    // A hook that lets `at` steps happen, then does `outcome` to the next and
    // lets the rest happen (none do after a crash).
    storage::ProjectFolder::StepHook at (std::size_t step, StepOutcome outcome)
    {
        return [step, outcome, taken = std::size_t { 0 }] (const Step&) mutable { return taken++ == step ? outcome : StepOutcome::proceed; };
    }

    // A crash at the nth step of a kind (from 0).
    storage::ProjectFolder::StepHook crashAt (Step::Kind kind, int nth = 0)
    {
        return [kind, nth, seen = 0] (const Step& step) mutable { return step.kind == kind && seen++ == nth ? StepOutcome::crash : StepOutcome::proceed; };
    }

    std::size_t firstOf (const std::vector<Step>& steps, Step::Kind kind)
    {
        const auto found = std::ranges::find (steps, kind, &Step::kind);
        REQUIRE (found != steps.end());
        return static_cast<std::size_t> (found - steps.begin());
    }

    // Rule 2: the texts, by a name for each, that no file in the folder holds.
    std::vector<std::string> lostTexts (const std::filesystem::path& folder, const std::map<std::string, std::string>& texts)
    {
        const auto files = filesIn (folder);
        std::vector<std::string> lost;

        for (const auto& [name, text] : texts)
            if (std::ranges::none_of (files, [&text] (const auto& file) { return file.second == text; }))
                lost.push_back (name);

        return lost;
    }

    // The texts of these, by name, that are on disk somewhere in the folder.
    std::map<std::string, std::string> textsOnDisk (const std::filesystem::path& folder, const std::map<std::string, std::string>& texts)
    {
        auto found = texts;
        std::erase_if (found, [lost = lostTexts (folder, texts)] (const auto& text) { return std::ranges::find (lost, text.first) != lost.end(); });
        return found;
    }

    // project.json and the clip files it names, as on disk; nullopt if it doesn't load.
    std::optional<std::map<std::string, std::string>> versionOnDisk (const std::filesystem::path& folder)
    {
        const auto result = project::load (folder);

        if (! result.project)
            return {};

        std::map<std::string, std::string> files;

        for (const auto& entry : project::write (*result.project))
            files[entry.first] = readTextFile (folder / pathFromUtf8 (entry.first)).value_or ("(can't be read)");

        return files;
    }

    // The steps a whole save takes, and the versions before and after it.
    struct WholeSave
    {
        std::vector<Step> steps;
        std::map<std::string, std::string> before, after; // project.json and the clip files it names
        project::Project beforeProject, afterProject;

        WholeSave()
        {
            SavedProject saved;
            beforeProject = saved.bus->project();
            before = project::write (beforeProject);
            makeChanges (*saved.bus);
            saved.projectFolder->setStepHook ([this] (const Step& step) { steps.push_back (step); return StepOutcome::proceed; });
            REQUIRE (saved.bus->save ({}, claude).ok);
            afterProject = saved.bus->project();
            after = project::write (afterProject);
            REQUIRE (before != after);
            REQUIRE (std::filesystem::exists (saved.file ("unused/clips/fill.notes")));
        }

        // Every text of both versions, by a name each.
        std::map<std::string, std::string> oldTexts() const { return named ("before: ", before); }
        std::map<std::string, std::string> newTexts() const { return named ("after: ", after); }

        static std::map<std::string, std::string> named (const std::string& prefix, const std::map<std::string, std::string>& files)
        {
            std::map<std::string, std::string> texts;

            for (const auto& [file, text] : files)
                texts[prefix + file] = text;

            return texts;
        }
    };

    // The steps finishing an interrupted save takes: interrupted as it began
    // putting files in place, so finishing has every kind of step to take.
    std::vector<Step> finishingSteps()
    {
        SavedProject saved;
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook (crashAt (Step::Kind::swap));
        REQUIRE_FALSE (saved.bus->save ({}, claude).ok);

        std::vector<Step> steps;
        const auto finished = storage::finishInterruptedSave (saved.folder, [&steps] (const Step& step) { steps.push_back (step); return StepOutcome::proceed; });
        REQUIRE (finished.outcome == Finished::finished);
        return steps;
    }

    // Rule 1, at the next open: it either finishes and plays a version whole,
    // before or after the save; or stops, with nothing on disk changed, and
    // says where the versions are. Returns what the open did.
    storage::FinishedSave checkNextOpen (const std::filesystem::path& folder, const WholeSave& whole)
    {
        const auto disk = filesIn (folder);
        const auto finished = storage::finishInterruptedSave (folder);
        CAPTURE (finished.message);

        if (finished.proceed())
        {
            const auto version = versionOnDisk (folder);
            REQUIRE (version);
            CHECK ((*version == whole.before || *version == whole.after));
            CHECK (leftoversIn (folder).empty());
        }
        else
        {
            CHECK (finished.outcome == Finished::blocked);
            CHECK (filesIn (folder) == disk);
            CHECK (contains (finished.message, "kept"));
            CHECK (contains (finished.message, "nothing on disk was changed, and nothing was played"));
        }

        return finished;
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

    // A save stopped by a crash as it began putting files in place.
    void interruptedAtFirstSwap (SavedProject& saved)
    {
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook (crashAt (Step::Kind::swap));
        REQUIRE_FALSE (saved.bus->save ({}, claude).ok);
        REQUIRE (storage::hasInterruptedSave (saved.folder));
    }

    // The kept save the interrupted save made, which holds the version before it.
    std::string keptFolderOf (SavedProject& saved)
    {
        const auto saves = saved.bus->keptSaves();
        REQUIRE_FALSE (saves.empty());
        return saves.back().folder;
    }

    // A save that can't be finished: nothing on disk changes, --play stops,
    // and the message says why and where both versions are; --check says so too.
    void checkBlocked (SavedProject& saved, std::string_view why, std::string_view whereOld)
    {
        const auto disk = filesIn (saved.folder);
        const auto finished = storage::finishInterruptedSave (saved.folder);
        CAPTURE (finished.message);

        CHECK (finished.outcome == Finished::blocked);
        CHECK_FALSE (finished.proceed());
        CHECK (filesIn (saved.folder) == disk);
        CHECK (contains (finished.message, "was interrupted part-way, and can't be finished as it meant, so nothing on disk was changed, and nothing was played: "));
        CHECK (contains (finished.message, why));
        CHECK (contains (finished.message, whereOld));
        CHECK (contains (finished.message, storage::temporarySuffix));
        CHECK (contains (finished.message, "delete .hocket/saving.json, the save's record: the next --play then plays the files as they are"));

        const auto note = storage::describeInterruptedSave (saved.folder);
        CHECK (note.starts_with ("Note: "));
        CHECK (contains (note, why));
        CHECK (contains (note, "so --play won't play it until that's sorted out"));
        CHECK (filesIn (saved.folder) == disk);
    }
}

//==============================================================================
TEST_CASE ("a save stopped at any step, as a crash stops it: the next open ends with the folder whole, as the save meant or as it was, and no text is lost")
{
    const WholeSave whole;
    const auto marker = firstOf (whole.steps, Step::Kind::nameMarker);

    for (std::size_t stop = 0; stop < whole.steps.size(); ++stop)
    {
        CAPTURE (stop);
        CAPTURE (kindName (whole.steps[stop].kind));
        CAPTURE (utf8FromPath (whole.steps[stop].path.filename()));

        SavedProject saved;
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook (at (stop, StepOutcome::crash));
        CHECK_FALSE (saved.bus->save ({}, claude).ok);

        // Past the marker, the save is under way; --check says so, changing nothing.
        const auto begun = stop > marker;
        CHECK (storage::hasInterruptedSave (saved.folder) == begun);
        const auto note = storage::describeInterruptedSave (saved.folder);
        CHECK ((begun ? contains (note, "--play finishes it before playing") : note.empty() || contains (note, "which --play sets aside in unused/")));

        // The session is gone with the crash: whatever new text reached the disk stays there.
        CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
        const auto newOnDisk = textsOnDisk (saved.folder, whole.newTexts());

        if (begun)
            CHECK (newOnDisk == whole.newTexts());

        const auto finished = checkNextOpen (saved.folder, whole);
        REQUIRE (finished.proceed());
        CHECK (versionOnDisk (saved.folder) == (begun ? whole.after : whole.before));
        CHECK (loaded (saved.folder) == (begun ? whole.afterProject : whole.beforeProject));
        CHECK_FALSE (storage::hasInterruptedSave (saved.folder));

        CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
        CHECK (lostTexts (saved.folder, newOnDisk).empty());
    }
}

TEST_CASE ("a save made to fail at any step fails cleanly, or saves, and both rules hold: at once, at the next open, and after the session saves again")
{
    const WholeSave whole;

    for (std::size_t failing = 0; failing < whole.steps.size(); ++failing)
    {
        for (const auto savesAgain : { false, true })
        {
            CAPTURE (failing);
            CAPTURE (kindName (whole.steps[failing].kind));
            CAPTURE (utf8FromPath (whole.steps[failing].path.filename()));
            CAPTURE (savesAgain);

            SavedProject saved;
            makeChanges (*saved.bus);
            saved.projectFolder->setStepHook (at (failing, StepOutcome::fail));
            const auto result = saved.bus->save ({}, claude);
            saved.projectFolder->setStepHook ({});
            CAPTURE (result.message);

            // Moving a file to unused/, and removing the marker, come once
            // every file is in place: the save is made.
            const auto kind = whole.steps[failing].kind;
            CHECK (result.ok == (kind == Step::Kind::moveUnused || kind == Step::Kind::removeMarker));
            CHECK (contains (result.message, "the step hook made it fail"));

            if (! result.ok)
                CHECK (contains (result.message, "Nothing on disk changed"));

            // The session has its project, and the disk every text of the version before.
            CHECK (saved.bus->project() == whole.afterProject);
            CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());

            if (savesAgain)
            {
                const auto again = saved.bus->save ({}, claude);
                CAPTURE (again.message);
                CHECK (again.ok);
                CHECK (versionOnDisk (saved.folder) == whole.after);
            }

            const auto finished = checkNextOpen (saved.folder, whole);
            CHECK (finished.proceed());
            CHECK (versionOnDisk (saved.folder) == (result.ok || savesAgain ? whole.after : whole.before));
            CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());

            if (result.ok || savesAgain)
                CHECK (lostTexts (saved.folder, whole.newTexts()).empty());
        }
    }
}

#ifdef _WIN32
TEST_CASE ("a save whose swap fails, and whose putting back fails at any step, leaves the marker: the next open stops, changing nothing, unless the session saves again")
{
    const WholeSave whole;
    const auto firstSwap = firstOf (whole.steps, Step::Kind::swap);

    // Swaps 1 to 3 fail with files already in place to put back; each put back is made to fail in turn.
    for (std::size_t swap = 1; swap < 4; ++swap)
    {
        for (std::size_t putBack = 0; putBack < swap; ++putBack)
        {
            for (const auto savesAgain : { false, true })
            {
                CAPTURE (swap);
                CAPTURE (putBack);
                CAPTURE (savesAgain);

                SavedProject saved;
                makeChanges (*saved.bus);
                saved.projectFolder->setStepHook ([swap, putBack, target = firstSwap + swap, taken = std::size_t { 0 }, putBacks = std::size_t { 0 }] (const Step& step) mutable
                {
                    if (taken++ == target)
                        return StepOutcome::fail;

                    if (step.kind == Step::Kind::putBack && putBacks++ == putBack)
                        return StepOutcome::fail;

                    return StepOutcome::proceed;
                });

                const auto result = saved.bus->save ({}, claude);
                saved.projectFolder->setStepHook ({});
                CAPTURE (result.message);

                CHECK_FALSE (result.ok);
                CHECK (contains (result.message, "so the folder is part new, part old: save again once the problem is fixed"));
                CHECK (storage::hasInterruptedSave (saved.folder));
                CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());

                if (savesAgain)
                {
                    // Its own marker doesn't stop it: the save puts the folder right.
                    const auto again = saved.bus->save ({}, claude);
                    CAPTURE (again.message);
                    CHECK (again.ok);
                    CHECK_FALSE (storage::hasInterruptedSave (saved.folder));
                    CHECK (versionOnDisk (saved.folder) == whole.after);
                }

                const auto finished = checkNextOpen (saved.folder, whole);
                CHECK (finished.proceed() == savesAgain);
                CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
            }
        }
    }
}
#endif

//==============================================================================
TEST_CASE ("finishing an interrupted save, stopped at any step, is finished by the open after")
{
    const WholeSave whole;
    const auto steps = finishingSteps();
    REQUIRE (steps.size() == 6); // four files put in place, one moved, the marker removed

    {
        SavedProject saved;
        interruptedAtFirstSwap (saved);
        CHECK (storage::describeInterruptedSave (saved.folder)
               == "Note: a save by Claude, begun " + localTimeText (std::chrono::floor<std::chrono::seconds> (saved.now))
                      + ", was interrupted part-way (.hocket/saving.json is there), so the folder may hold part of it. --play finishes it before playing.");

        const auto finished = storage::finishInterruptedSave (saved.folder);
        CHECK (finished.outcome == Finished::finished);
        CHECK (finished.message
               == "Finished a save by Claude, begun " + localTimeText (std::chrono::floor<std::chrono::seconds> (saved.now))
                      + ", which was interrupted part-way: wrote clips/bass.notes, clips/verse.notes and project.json; moved clips/fill.notes to "
                        "unused/clips/fill.notes, since no clip uses it any more.");
        CHECK (versionOnDisk (saved.folder) == whole.after);
    }

    for (std::size_t stop = 0; stop < steps.size(); ++stop)
    {
        CAPTURE (stop);
        CAPTURE (kindName (steps[stop].kind));

        SavedProject saved;
        interruptedAtFirstSwap (saved);

        const auto finished = storage::finishInterruptedSave (saved.folder, at (stop, StepOutcome::crash));
        CHECK (finished.message == "stopped part-way by the step hook");
        CHECK_FALSE (finished.proceed());
        CHECK (storage::hasInterruptedSave (saved.folder));
        CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
        CHECK (lostTexts (saved.folder, whole.newTexts()).empty());

        checkNextOpen (saved.folder, whole);
        CHECK (versionOnDisk (saved.folder) == whole.after);
        CHECK (loaded (saved.folder) == whole.afterProject);
    }
}

TEST_CASE ("finishing made to fail at any step stops --play there: what's done stays, no text is lost, and the next open finishes the rest")
{
    const WholeSave whole;
    const auto steps = finishingSteps();

    for (std::size_t failing = 0; failing < steps.size(); ++failing)
    {
        CAPTURE (failing);
        CAPTURE (kindName (steps[failing].kind));

        SavedProject saved;
        interruptedAtFirstSwap (saved);

        const auto finished = storage::finishInterruptedSave (saved.folder, at (failing, StepOutcome::fail));
        CAPTURE (finished.message);
        CHECK (finished.outcome == Finished::failed);
        CHECK_FALSE (finished.proceed());
        CHECK (contains (finished.message, "and finishing it failed "));
        CHECK (contains (finished.message, "(the step hook made it fail), so nothing was played. What it did stays done, and the next --play finishes the rest."));
        CHECK (storage::hasInterruptedSave (saved.folder));
        CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
        CHECK (lostTexts (saved.folder, whole.newTexts()).empty());

        // Nothing saves over it meanwhile: a session that opened the folder anyway can't.
        storage::ProjectFolder another (saved.folder, project::Project {});
        CHECK (contains (another.save (whole.afterProject, {}, lee).message, "a save interrupted earlier isn't finished"));

        const auto again = checkNextOpen (saved.folder, whole);
        CHECK (again.outcome == Finished::finished);
        CHECK (versionOnDisk (saved.folder) == whole.after);
        CHECK (lostTexts (saved.folder, whole.newTexts()).empty());
    }
}

TEST_CASE ("what a save stopped before its marker left is set aside in unused/, never deleted; setting it aside, stopped or made to fail at any step, is done by the open after")
{
    const WholeSave whole;

    // A crash just before the marker went in place: every new text is beside its file.
    const auto leaveLeftovers = [] (SavedProject& saved)
    {
        makeChanges (*saved.bus);
        saved.projectFolder->setStepHook (crashAt (Step::Kind::nameMarker));
        REQUIRE_FALSE (saved.bus->save ({}, claude).ok);
        REQUIRE_FALSE (storage::hasInterruptedSave (saved.folder));
    };

    std::vector<Step> steps;

    {
        SavedProject saved;
        leaveLeftovers (saved);
        const auto leftovers = textsOnDisk (saved.folder, whole.newTexts());
        CHECK (leftovers == whole.newTexts()); // bass, verse and project.json beside their files; beat.notes as it was

        CHECK (storage::describeInterruptedSave (saved.folder)
               == "Note: a save was interrupted before it changed anything, and left project.json.hocket-saving, .hocket/save.json.hocket-saving, "
                  ".hocket/saving.json.hocket-saving, clips/bass.notes.hocket-saving and clips/verse.notes.hocket-saving, which --play sets aside in unused/.");

        const auto finished = storage::finishInterruptedSave (saved.folder, [&steps] (const Step& step) { steps.push_back (step); return StepOutcome::proceed; });

        CHECK (finished.outcome == Finished::setAside);
        CHECK (finished.proceed());
        CHECK (finished.message
               == "A save was interrupted before it changed anything. What it left, whose texts may be nowhere else, is set aside: project.json.hocket-saving to "
                  "unused/project.json.hocket-saving, .hocket/save.json.hocket-saving to unused/.hocket/save.json.hocket-saving, .hocket/saving.json.hocket-saving to "
                  "unused/.hocket/saving.json.hocket-saving, clips/bass.notes.hocket-saving to unused/clips/bass.notes.hocket-saving and "
                  "clips/verse.notes.hocket-saving to unused/clips/verse.notes.hocket-saving.");
        CHECK (versionOnDisk (saved.folder) == whole.before);
        CHECK (leftoversIn (saved.folder).empty());
        CHECK (lostTexts (saved.folder, leftovers).empty());
        CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
    }

    REQUIRE (steps.size() == 5);

    for (const auto outcome : { StepOutcome::crash, StepOutcome::fail })
    {
        for (std::size_t step = 0; step < steps.size(); ++step)
        {
            CAPTURE (outcome == StepOutcome::crash ? "crash" : "fail");
            CAPTURE (step);

            SavedProject saved;
            leaveLeftovers (saved);
            const auto leftovers = textsOnDisk (saved.folder, whole.newTexts());

            const auto finished = storage::finishInterruptedSave (saved.folder, at (step, outcome));
            CHECK (finished.outcome == Finished::failed);
            CHECK_FALSE (finished.proceed());
            CHECK (lostTexts (saved.folder, leftovers).empty());

            checkNextOpen (saved.folder, whole);
            CHECK (versionOnDisk (saved.folder) == whole.before);
            CHECK (lostTexts (saved.folder, leftovers).empty());
        }
    }
}

//==============================================================================
// Can't be finished: nothing on disk changes, and --play stops, saying where both versions are.

TEST_CASE ("review 2: a marker that can't be read changes nothing: the save's new texts stay, and --play stops, saying where both versions are")
{
    const WholeSave whole;
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    const auto disk = filesIn (saved.folder);
    const auto savesBass = *readTextFile (saved.file (temporaryOf ("clips/bass.notes")));
    storage::FinishedSave finished;

    {
        // Something holds the marker as --play starts (a backup or sync tool, say).
        const auto held = platform::FileLock::acquire (saved.file (std::string (storage::savingMarkerFile)));
        REQUIRE (held);
        finished = storage::finishInterruptedSave (saved.folder);
    }

    CAPTURE (finished.message);
    CHECK (finished.outcome == Finished::blocked);
    CHECK (filesIn (saved.folder) == disk);
    CHECK_FALSE (filesHolding (saved.folder, savesBass).empty());
    CHECK (contains (finished.message, ".hocket/saving.json, its record of what goes where, is there, but it can't be read (another program may be holding it)"));
    CHECK (contains (finished.message, "The version before the save is most likely the newest kept save, save #1"));
    CHECK (contains (finished.message, "The save's new texts not yet in place are beside their files: project.json.hocket-saving, .hocket/save.json.hocket-saving, "
                                       "clips/bass.notes.hocket-saving and clips/verse.notes.hocket-saving."));

    // Once it's let go, the next open finishes the save.
    checkNextOpen (saved.folder, whole);
    CHECK (versionOnDisk (saved.folder) == whole.after);
}

TEST_CASE ("a marker that doesn't read as one changes nothing, and --play stops")
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    writeFile (saved.file (std::string (storage::savingMarkerFile)), "{ \"format\": \"hocket-saving\", \"files\": [");

    checkBlocked (saved, "is there, but it doesn't read as a save's record (it may have been changed by hand, or damaged)", "the newest kept save, save #1");
}

TEST_CASE ("review 3: a file the save replaces that changed since it stopped stops finishing: project.json written again by hand, say")
{
    SavedProject saved;
    const auto oldProjectJson = *readTextFile (saved.file ("project.json"));
    interruptedAtFirstSwap (saved);

    // After the crash, before the next --play: project.json written again by
    // hand (a checkout, say), still naming clips/fill.notes.
    writeFile (saved.file ("project.json"), oldProjectJson + "\n");

    checkBlocked (saved, ": project.json changed after the save stopped.", "The version before the save is kept whole in '" + keptFolderOf (saved) + "'.");
    CHECK (std::filesystem::exists (saved.file ("clips/fill.notes")));
    CHECK (project::load (saved.folder).project);
}

TEST_CASE ("a file the save had put in place, changed since, stops finishing")
{
    SavedProject saved;
    makeChanges (*saved.bus);
    saved.projectFolder->setStepHook (crashAt (Step::Kind::swap, 1)); // bass in place
    REQUIRE_FALSE (saved.bus->save ({}, claude).ok);
    writeFile (saved.file ("clips/bass.notes"), byHand);

    checkBlocked (saved, "clips/bass.notes isn't as the save would have put it in place, and its new text, clips/bass.notes.hocket-saving, is gone",
                  "kept whole in '" + keptFolderOf (saved) + "'");
}

TEST_CASE ("a new text missing from beside its file stops finishing")
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    std::filesystem::remove (saved.file (temporaryOf ("clips/verse.notes")));

    checkBlocked (saved, "clips/verse.notes isn't as the save would have put it in place, and its new text, clips/verse.notes.hocket-saving, is gone",
                  "kept whole in '" + keptFolderOf (saved) + "'");
}

TEST_CASE ("a new text changed beside its file stops finishing")
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    writeFile (saved.file (temporaryOf ("clips/verse.notes")), byHand);

    checkBlocked (saved, "clips/verse.notes.hocket-saving, the save's new text, isn't as the save wrote it", "kept whole in '" + keptFolderOf (saved) + "'");
}

TEST_CASE ("finishing never replaces a file whose text isn't in the kept save")
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    const auto kept = keptFolderOf (saved);
    std::filesystem::remove (saved.file (kept + "/clips/bass.notes"));

    checkBlocked (saved, "the kept save '" + kept + "' doesn't hold clips/bass.notes as it is, so putting the save's text in its place would lose it",
                  "kept whole in '" + kept + "'");
}

TEST_CASE ("a clip file the save was moving to unused/, changed since, stops finishing")
{
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    writeFile (saved.file ("clips/fill.notes"), "@length 1bar\n\n1.1 [kick] 1/16 v120\n");

    checkBlocked (saved, "clips/fill.notes, which the save was moving to unused/ since no clip used it, changed after the save stopped",
                  "kept whole in '" + keptFolderOf (saved) + "'");
}

TEST_CASE ("a marker never touches a file that isn't one of the project's: it doesn't read as a save's record")
{
    SavedProject saved;
    writeFile (saved.folder.parent_path() / pathFromUtf8 ("elsewhere.txt"), "not the project's");
    writeFile (saved.file ("../elsewhere.txt" + std::string (storage::temporarySuffix)), "a damaged marker's");
    writeFile (saved.file (std::string (storage::savingMarkerFile)),
               "{ \"format\": \"hocket-saving\", \"author\": \"Lee\", \"time\": \"2026-11-20T09:15:00Z\", \"kept\": \"\", \"files\": [ { \"file\": \"../elsewhere.txt\", "
               "\"before\": { \"exists\": true, \"modified\": 0, \"size\": 0 }, \"after\": { \"exists\": true, \"modified\": 0, \"size\": 0 } } ], \"unused\": [] }");

    CHECK (storage::hasInterruptedSave (saved.folder));
    checkBlocked (saved, "doesn't read as a save's record", "There's no kept save.");
    CHECK (readTextFile (saved.folder.parent_path() / pathFromUtf8 ("elsewhere.txt")) == "not the project's");
}

TEST_CASE ("once the person deletes the marker of a save that couldn't be finished, --play plays the files as they are, setting the save's texts aside")
{
    SavedProject saved;
    const auto oldProjectJson = *readTextFile (saved.file ("project.json"));
    interruptedAtFirstSwap (saved);
    const auto savesBass = *readTextFile (saved.file (temporaryOf ("clips/bass.notes")));
    writeFile (saved.file ("project.json"), oldProjectJson + "\n");
    REQUIRE (storage::finishInterruptedSave (saved.folder).outcome == Finished::blocked);

    std::filesystem::remove (saved.file (std::string (storage::savingMarkerFile)));
    const auto finished = storage::finishInterruptedSave (saved.folder);

    CHECK (finished.outcome == Finished::setAside);
    CHECK (readTextFile (saved.file ("unused/clips/bass.notes.hocket-saving")) == savesBass);
    CHECK (leftoversIn (saved.folder).empty());
    CHECK (project::load (saved.folder).project);
}

//==============================================================================
#ifdef _WIN32
TEST_CASE ("review 1: finishing that fails stops --play, and while its marker is there no save writes over what it was putting in place")
{
    const WholeSave whole;
    SavedProject saved;
    interruptedAtFirstSwap (saved);
    const auto savesBass = *readTextFile (saved.file (temporaryOf ("clips/bass.notes")));
    storage::FinishedSave finished;

    {
        // Another program has clips/bass.notes open as --play starts, as some
        // editors do: readable, but Windows won't replace it.
        std::ifstream held (saved.file ("clips/bass.notes"), std::ios::binary);
        REQUIRE (held);
        finished = storage::finishInterruptedSave (saved.folder);
    }

    CAPTURE (finished.message);
    CHECK (finished.outcome == Finished::failed);
    CHECK_FALSE (finished.proceed());
    CHECK (contains (finished.message, "and finishing it failed putting clips/bass.notes in place ("));
    CHECK (storage::hasInterruptedSave (saved.folder));

    // Were a session to open the folder anyway, its saves would be refused.
    saved.open();
    const auto before = filesIn (saved.folder);
    REQUIRE (saved.bus->execute (SetTempo { 120 }, lee).ok);
    const auto refused = saved.bus->save ({}, lee);

    CHECK_FALSE (refused.ok);
    CHECK (refused.message
           == "didn't save: a save interrupted earlier isn't finished: .hocket/saving.json is there, describing it, and saving now could write over what it "
              "was putting in place. Nothing on disk changed. Start --play again: it finishes that save before it reads the folder, or says why it can't "
              "and where its files are");
    CHECK (filesIn (saved.folder) == before);

    // The next --play finishes it.
    checkNextOpen (saved.folder, whole);
    CHECK (versionOnDisk (saved.folder) == whole.after);
    CHECK (readTextFile (saved.file ("clips/bass.notes")) == savesBass);
}
#endif

TEST_CASE ("a save sets aside a temporary file an earlier save left where it writes, never writing over it")
{
    SavedProject saved;
    const std::string leftover = "@length 4bars\n\n1.1 D2 1/4 v80\n";
    writeFile (saved.file (temporaryOf ("clips/bass.notes")), leftover);
    REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, claude).ok);

    const auto result = saved.bus->save ({}, lee);

    CHECK (result.ok);
    CHECK (contains (result.message, "set aside clips/bass.notes.hocket-saving to unused/clips/bass.notes.hocket-saving, which an earlier save left when it was interrupted"));
    CHECK (readTextFile (saved.file ("unused/clips/bass.notes.hocket-saving")) == leftover);
    CHECK (loaded (saved.folder) == saved.bus->project());
}

TEST_CASE ("a save that finds an earlier save's leftovers in its way, stopped or made to fail at any step, loses none of their texts, nor the version before it")
{
    const WholeSave whole;
    const std::string leftoverBass = "@length 4bars\n\n1.1 D2 1/4 v80\n";
    const std::string partMade = "{ \"format\": \"hocket-project\", part-made";
    const std::map<std::string, std::string> leftovers { { "bass's temporary file", leftoverBass }, { "a part-made copy's project.json", partMade } };

    // A temporary file where the save writes bass's, and a part-made copy where it makes its kept save.
    const auto plant = [&] (SavedProject& saved)
    {
        writeFile (saved.file (temporaryOf ("clips/bass.notes")), leftoverBass);
        writeFile (saved.file (".hocket/saves/saving 1/project.json"), partMade);
        makeChanges (*saved.bus);
    };

    std::vector<Step> steps;

    {
        SavedProject saved;
        plant (saved);
        saved.projectFolder->setStepHook ([&steps] (const Step& step) { steps.push_back (step); return StepOutcome::proceed; });
        const auto result = saved.bus->save ({}, claude);
        CAPTURE (result.message);
        REQUIRE (result.ok);
        CHECK (contains (result.message, "set aside clips/bass.notes.hocket-saving to unused/clips/bass.notes.hocket-saving"));
        CHECK (readTextFile (saved.file ("unused/.hocket/saves/saving 1/project.json")) == partMade);
        CHECK (lostTexts (saved.folder, leftovers).empty());
    }

    REQUIRE (std::ranges::count (steps, Step::Kind::setAside, &Step::kind) == 2);

    for (const auto outcome : { StepOutcome::crash, StepOutcome::fail })
    {
        for (std::size_t step = 0; step < steps.size(); ++step)
        {
            CAPTURE (outcome == StepOutcome::crash ? "crash" : "fail");
            CAPTURE (step);
            CAPTURE (kindName (steps[step].kind));

            SavedProject saved;
            plant (saved);
            saved.projectFolder->setStepHook (at (step, outcome));
            const auto result = saved.bus->save ({}, claude);
            saved.projectFolder->setStepHook ({});
            CAPTURE (result.message);

            CHECK (lostTexts (saved.folder, leftovers).empty());
            CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());

            checkNextOpen (saved.folder, whole);
            CHECK (lostTexts (saved.folder, leftovers).empty());
            CHECK (lostTexts (saved.folder, whole.oldTexts()).empty());
        }
    }
}
