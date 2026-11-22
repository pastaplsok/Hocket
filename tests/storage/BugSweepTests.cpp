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
