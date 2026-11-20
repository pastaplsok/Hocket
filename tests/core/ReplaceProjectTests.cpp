// Replacing the whole project (ReplaceProject), and reverting to a kept save
// with it (SPEC §4.5).

#include "BusTesting.h"
#include "common/RandomCommands.h"

#include <doctest/doctest.h>

#include <functional>
#include <set>
#include <sstream>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    // The project after up to `count` random commands (never a replacement).
    project::Project evolved (Rng& rng, project::Project project, int count, kits::KitCatalog& kits)
    {
        CommandBus bus (std::move (project));
        bus.setKitCatalog (&kits);

        for (int i = 0; i < count; ++i)
            (void) bus.execute (randomCommand (rng, bus.project(), true, false), lee);

        return bus.project();
    }

    std::vector<std::string> words (const std::string& text)
    {
        std::istringstream in (text);
        std::vector<std::string> list;

        for (std::string word; in >> word;)
            list.push_back (word);

        return list;
    }

    // A track that's the same in both projects, with the same notes in its
    // files, gets no change notice, nor do its clips or files: only a
    // TrackMoved, when others moved around it. So the engine leaves it alone.
    void checkUntouched (const project::Project& from, const project::Project& to, const std::vector<std::string>& changes)
    {
        for (const auto& track : from.tracks)
        {
            const auto* same = project::findTrack (to, track.id);

            if (same == nullptr || ! (*same == track))
                continue;

            std::set<std::string> names { track.id };
            auto filesSame = true;

            for (const auto& clip : track.clips)
            {
                names.insert (clip.id);
                names.insert (clip.file);
                filesSame = filesSame && from.notesFiles.at (clip.file) == to.notesFiles.at (clip.file);
            }

            if (! filesSame)
                continue;

            for (const auto& change : changes)
            {
                const auto said = words (change);

                if (said.front() == "TrackMoved")
                    continue;

                for (const auto& word : said)
                    CHECK_MESSAGE (! names.contains (word), "track " << track.id << " is the same in both, but got " << change);
            }
        }
    }
}

TEST_CASE ("a replacement gets to the new project exactly, touching no track it doesn't change, and undo and redo go back and forth exactly")
{
    int replaced = 0;

    for (std::uint32_t seed = 1; seed <= 300; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        auto kits = randomSequenceKits();

        // A few commands apart, or two sequences apart from the start.
        const auto from = evolved (rng, randomSequenceProject(), randomInt (rng, 0, 12), kits);
        const auto to = evolved (rng, chance (rng, 50) ? from : randomSequenceProject(), randomInt (rng, 1, 12), kits);

        CommandBus bus (from);
        ChangeLog log (bus);
        const auto result = bus.execute (ReplaceProject { to, {} }, claude);
        CAPTURE (result.message);
        REQUIRE (result.ok);

        if (project::write (to) == project::write (from))
        {
            CHECK (bus.history().empty());
            continue;
        }

        ++replaced;
        REQUIRE (bus.project() == to);
        REQUIRE (filesMatchClips (bus.project()));
        checkUntouched (from, to, log.take());

        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        REQUIRE (bus.project() == from);
        REQUIRE (bus.redo (lee).ok);
        REQUIRE (bus.project() == to);
    }

    MESSAGE ("replacements: " << replaced);
    CHECK (replaced >= 250);
}

TEST_CASE ("a replacement is one history entry, which says what it changed")
{
    auto to = randomSequenceProject();
    to.tempo = 130;
    to.tracks.erase (to.tracks.begin() + 1); // trk-lead
    to.notesFiles["clips/b.notes"] = parsedNotes ("@length 1bar\n\n1.1 A1 1/4 v100\n");
    to.tracks.front().volume = -6;

    CommandBus bus (randomSequenceProject());
    const auto result = bus.execute (ReplaceProject { to, "read project.json" }, lee);

    REQUIRE (result.ok);
    CHECK (result.message == "#1 read project.json. Changes: the tempo (120 BPM to 130 BPM); removed trk-lead; changed trk-bass; rewrote the notes of 1 clip file");
    CHECK (bus.history().size() == 1);
    CHECK (bus.project() == to);

    SUBCASE ("without a description")
    {
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        CHECK (testing::contains (bus.execute (ReplaceProject { to, {} }, lee).message, "#2 replaced the project. Changes: "));
    }

    SUBCASE ("only the order of the tracks")
    {
        auto reordered = to;
        std::ranges::reverse (reordered.tracks);
        CHECK (bus.execute (ReplaceProject { reordered, {} }, lee).message == "#2 replaced the project. Changes: the order of the tracks");
        CHECK (bus.project() == reordered);
    }
}

TEST_CASE ("a replacement with the same project, as written, changes nothing")
{
    const auto start = randomSequenceProject();
    CommandBus bus (start);

    SUBCASE ("the same")
    {
        const auto result = bus.execute (ReplaceProject { start, {} }, lee);
        CHECK (result.ok);
        CHECK (result.message == "the project is already the same as the new one, so nothing changed and no history entry was added");
    }

    SUBCASE ("its clips kept in another order, which writes out the same")
    {
        auto reordered = start;
        std::ranges::reverse (reordered.tracks.front().clips);
        CHECK (bus.execute (ReplaceProject { reordered, {} }, lee).ok);
    }

    CHECK (bus.history().empty());
    CHECK (bus.project() == start);
}

TEST_CASE ("a replacement with a project that isn't valid is refused, saying why, and changes nothing")
{
    const auto start = randomSequenceProject();

    struct Case
    {
        const char* what;
        std::function<void (project::Project&)> breakIt;
        std::string says;
    };

    const Case cases[] = {
        { "a file no clip uses", [] (project::Project& p) { p.notesFiles["clips/stray.notes"] = parsedNotes ("@length 1bar\n"); },
          "it has notes for clips/stray.notes, which no clip uses" },
        { "a clip's file missing", [] (project::Project& p) { p.tracks.front().clips.push_back ({ "clp-x", "clips/missing.notes", 16 * 3840 }); },
          "clips/missing.notes" },
        { "overlapping clips", [] (project::Project& p) { p.tracks.front().clips.push_back ({ "clp-x", "clips/b.notes", 3840 }); }, "overlap" },
        { "a tempo too fast", [] (project::Project& p) { p.tempo = 400; }, "tempo" },
        { "a track ID twice", [] (project::Project& p) { p.tracks.push_back (p.tracks.back()); }, "trk-drums" },
    };

    for (const auto& c : cases)
    {
        CAPTURE (c.what);
        auto broken = start;
        c.breakIt (broken);
        CommandBus bus (start);

        const auto result = bus.execute (ReplaceProject { broken, {} }, lee);

        CHECK_FALSE (result.ok);
        CHECK (testing::contains (result.message, "replace the project: the new project isn't valid: "));
        CHECK_MESSAGE (testing::contains (result.message, c.says), result.message);
        CHECK (bus.project() == start);
        CHECK (bus.history().empty());
    }
}

//==============================================================================
TEST_CASE ("revert brings back a kept save as one history entry, which undo takes back")
{
    FakeStorage storage;
    auto earlier = randomSequenceProject();
    earlier.tempo = 96;
    earlier.tracks.pop_back(); // no drums
    earlier.notesFiles.erase ("clips/beat.notes");
    storage.keep (1, randomSequenceProject());
    storage.keep (2, earlier, "before the drums");

    CommandBus bus (randomSequenceProject());
    bus.setStorage (&storage);
    const auto now = bus.project();

    const auto result = bus.execute (Revert { 2 }, claude);
    const auto described = describe (storage.keptSaves()[1]);

    REQUIRE (result.ok);
    CHECK (result.message == "#1 reverted to " + described + ". Changes: the tempo (120 BPM to 96 BPM); removed trk-drums");
    CHECK (testing::contains (described, "save #2, by Lee, "));
    CHECK (testing::contains (described, ", \"before the drums\""));
    CHECK (bus.project() == earlier);
    CHECK (bus.hasUnsavedChanges());

    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.project() == now);
    CHECK_FALSE (bus.hasUnsavedChanges());

    SUBCASE ("to the save it already is")
    {
        const auto again = bus.execute (Revert { 1 }, claude);
        CHECK (again.ok);
        CHECK (again.message == "the project is already as " + describe (storage.keptSaves()[0]) + " has it, so nothing changed and no history entry was added");
        CHECK (bus.history().empty());
    }
}

TEST_CASE ("revert says why it can't")
{
    SUBCASE ("no storage")
    {
        CommandBus bus (randomSequenceProject());
        CHECK (bus.execute (Revert { 1 }, lee).message == "revert: this session has no project folder, so there are no kept saves to go back to");
    }

    FakeStorage storage;
    CommandBus bus (randomSequenceProject());
    bus.setStorage (&storage);

    SUBCASE ("none kept yet")
    {
        CHECK (bus.execute (Revert { 1 }, lee).message
               == "revert: there are no kept saves yet. Each save keeps the version it replaces, so there's one to go back to after the next save");
    }

    SUBCASE ("no such save")
    {
        storage.keep (1, randomSequenceProject());
        storage.keep (2, randomSequenceProject());
        storage.keep (4, randomSequenceProject());
        CHECK (bus.execute (Revert { 3 }, lee).message == "revert: there's no save #3; the kept saves are #1, #2 and #4");
    }

    SUBCASE ("a save that can't be read")
    {
        storage.keep (1, randomSequenceProject());
        storage.kept[0].loaded = { {}, { { "clips/a.notes", 3, 1, "'H2' isn't a note" }, { "project.json", 0, 0, "tempo is missing" } } };
        CHECK (bus.execute (Revert { 1 }, lee).message
               == "revert: " + describe (storage.keptSaves()[0]) + " can't be read, in .hocket/saves/1: clips/a.notes:3:1: 'H2' isn't a note; project.json: tempo is missing");
    }

    CHECK (bus.history().empty());
    CHECK (bus.project() == randomSequenceProject());
}

TEST_CASE ("a save's name is one line of text")
{
    FakeStorage storage;
    CommandBus bus (randomSequenceProject());
    bus.setStorage (&storage);

    for (const std::string name : { std::string ("two\nlines"), std::string (101, 'x'), std::string ("tab\there") })
    {
        CAPTURE (name);
        const auto result = bus.save ({ name }, lee);
        CHECK_FALSE (result.ok);
        CHECK (testing::contains (result.message, "didn't save: a save's name is one line of text, 1 to 100 bytes with no control characters"));
    }

    CHECK (storage.saves.empty());
    REQUIRE (bus.save ({ "before the bridge: take 2 ✓" }, lee).ok);
    CHECK (storage.saves.back().name == "before the bridge: take 2 ✓");
}
