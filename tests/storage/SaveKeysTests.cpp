// Saving from --play's console (SPEC §4.5): S saves as the person; a save
// refused for files changed outside Hocket offers O, which saves over them,
// only straight after that refusal and never on a second S (Lee, 2026-11-21).

#include "StorageTesting.h"

#include "storage/SaveKeys.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    const std::string newBass = "@length 4bars\n\n1.1 C2 1/2 v100\n";
    const std::string byHand = "@length 4bars\n\n1.1    C2 1/4 v100\n"; // as an editor might leave it

    const std::string notForNow = "O saves over files changed outside Hocket, only straight after S was refused for them. S saves.";

    // A change in the session, and the same clip file edited by hand on disk.
    void changeInsideAndOut (SavedProject& saved)
    {
        REQUIRE (saved.bus->execute (WriteClip { "clp-bass", newBass }, lee).ok);
        writeFile (saved.file ("clips/bass.notes"), byHand);
    }

    const std::string refusedForBass = "Didn't save, so as not to overwrite a file changed since Hocket last read or wrote it: clips/bass.notes changed "
                                       "outside Hocket. Nothing on disk changed.\nPress O to save over it anyway: what's on disk is kept as a kept save "
                                       "first, so it can be got back. Any other key leaves it as it is.";
}

TEST_CASE ("S saves as the person at the console, in either case, and says what it wrote")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);

    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok);
    CHECK (keys.press (U's') == "Saved: wrote project.json; the version it replaced is kept as save #1, by Lee, " + localTimeText (firstSaved));
    CHECK_FALSE (saved.bus->hasUnsavedChanges());
    CHECK (saved.bus->keptSaves().size() == 1);

    REQUIRE (saved.bus->execute (SetTempo { 110 }, claude).ok);
    const auto said = keys.press (U'S');
    REQUIRE (said);
    CHECK (said->starts_with ("Saved: wrote project.json; the version it replaced is kept as save #2, by Lee, "));
    CHECK (contains (*readTextFile (saved.file (".hocket/save.json")), "\"author\": \"Lee\""));

    CHECK (keys.press (U's') == "Saved: the files on disk already matched the project, so nothing was written");
}

TEST_CASE ("other keys say nothing, and O alone says what it's for")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);

    CHECK_FALSE (keys.press (U'x'));
    CHECK_FALSE (keys.press (U' '));
    CHECK (keys.press (U'o') == notForNow);
    CHECK (keys.press (U'O') == notForNow);
}

TEST_CASE ("a save refused for a file changed outside Hocket says which and offers O, which saves over it, keeping it first")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);
    changeInsideAndOut (saved);
    const auto before = filesIn (saved.folder);

    CHECK (keys.press (U's') == refusedForBass);
    CHECK (filesIn (saved.folder) == before);
    CHECK (saved.bus->hasUnsavedChanges());

    const auto said = keys.press (U'O');
    REQUIRE (said);
    CHECK (said->starts_with ("Saved: wrote clips/bass.notes; the version it replaced is kept as save #1, changed outside Hocket, "));
    CHECK (*readTextFile (saved.file ("clips/bass.notes")) == project::write (saved.bus->project()).at ("clips/bass.notes"));
    CHECK_FALSE (saved.bus->hasUnsavedChanges());

    // What was on disk, kept.
    const auto kept = saved.bus->keptSaves();
    REQUIRE (kept.size() == 1);
    CHECK (kept.front().info.changedOutside);
    CHECK (*readTextFile (saved.file (kept.front().folder + "/clips/bass.notes")) == byHand);

    // The offer is used up.
    CHECK (keys.press (U'o') == notForNow);
}

TEST_CASE ("S again after a refusal never saves over the changes: it's refused again, with the offer again")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);
    changeInsideAndOut (saved);
    const auto before = filesIn (saved.folder);

    CHECK (keys.press (U's') == refusedForBass);
    CHECK (keys.press (U's') == refusedForBass);
    CHECK (keys.press (U'S') == refusedForBass);
    CHECK (filesIn (saved.folder) == before);
}

TEST_CASE ("any other key after a refusal leaves the files as they are, and O no longer saves over them")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);
    changeInsideAndOut (saved);
    const auto before = filesIn (saved.folder);

    CHECK (keys.press (U's') == refusedForBass);
    CHECK (keys.press (U'n') == "Left the files changed outside Hocket as they are; nothing was saved.");
    CHECK (keys.press (U'o') == notForNow);
    CHECK (filesIn (saved.folder) == before);
}

TEST_CASE ("the offer lapses once the project changes: O doesn't save, and says to try S again")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);
    changeInsideAndOut (saved);
    const auto before = filesIn (saved.folder);

    CHECK (keys.press (U's') == refusedForBass);
    REQUIRE (saved.bus->execute (SetTempo { 100 }, claude).ok); // from the API, meanwhile
    CHECK (keys.press (U'o')
           == "The project changed after that save was refused, so O doesn't save over the files changed outside Hocket. Press S to try again.");
    CHECK (keys.press (U'o') == notForNow);
    CHECK (filesIn (saved.folder) == before);

    // S asks afresh, naming what's changed outside now.
    const auto said = keys.press (U's');
    REQUIRE (said);
    CHECK (said->starts_with ("Didn't save, so as not to overwrite a file changed since Hocket last read or wrote it: clips/bass.notes changed outside Hocket."));
}

TEST_CASE ("several files changed outside Hocket are named, each by how it changed")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);
    changeInsideAndOut (saved);
    REQUIRE (saved.bus->execute (WriteClip { "clp-fill", "@length 1bar\n\n1.1 snare 1/16 v90\n" }, lee).ok);
    std::filesystem::remove (saved.file ("clips/fill.notes"));

    CHECK (keys.press (U's')
           == "Didn't save, so as not to overwrite files changed since Hocket last read or wrote them: clips/bass.notes changed outside Hocket; "
              "clips/fill.notes was deleted outside Hocket. Nothing on disk changed.\nPress O to save over them anyway: what's on disk is kept as a "
              "kept save first, so it can be got back. Any other key leaves them as they are.");
}

TEST_CASE ("a save that fails for another reason offers nothing")
{
    SavedProject saved;
    storage::SaveKeys keys (*saved.bus, lee);
    writeFile (saved.file (".hocket/saves"), "a file where the kept saves go");
    REQUIRE (saved.bus->execute (SetTempo { 100 }, lee).ok);

    const auto said = keys.press (U's');
    REQUIRE (said);
    CHECK (said->starts_with ("Didn't save: couldn't keep the version on disk before replacing it: "));
    CHECK (keys.press (U'o') == notForNow);
    CHECK_FALSE (keys.press (U'x'));
}
