// The bus's save command, and how it knows what's unsaved (SPEC §4.5).

#include "BusTesting.h"

#include <doctest/doctest.h>

using namespace hocket;
using namespace hocket::commands;

namespace
{
    // One track with one clip.
    project::Project makeProject()
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::synth } }, {} };
        project.notesFiles["clips/a.notes"] = testing::parsedNotes ("@length 1bar\n\n1.1 C2 1/4 v100\n");
        bass.clips.push_back ({ "clp-a", "clips/a.notes", 0 });
        project.tracks.push_back (std::move (bass));
        return project;
    }

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    struct Fixture
    {
        Fixture() { bus.setStorage (&storage); }

        testing::FakeStorage storage;
        CommandBus bus { makeProject() };
    };
}

TEST_CASE ("the project a bus starts with counts as saved; a change makes it unsaved, and saving clears that")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    CHECK_FALSE (bus.hasUnsavedChanges());

    REQUIRE (bus.execute (SetTempo { 130 }, claude).ok);
    CHECK (bus.hasUnsavedChanges());

    const auto saved = bus.save ({}, lee);

    CHECK (saved.ok);
    CHECK (saved.message == "saved");
    CHECK_FALSE (bus.hasUnsavedChanges());
    REQUIRE (fixture.storage.saves.size() == 1);
    CHECK (fixture.storage.saves[0].project == bus.project());
    CHECK (fixture.storage.saves[0].author == lee);
}

TEST_CASE ("undoing or redoing back to the saved state counts as saved")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    REQUIRE (bus.execute (SetTempo { 130 }, lee).ok); // #1
    REQUIRE (bus.save ({}, lee).ok);
    REQUIRE (bus.execute (SetTempo { 140 }, lee).ok); // #2
    CHECK (bus.hasUnsavedChanges());

    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok); // back to #1, as saved
    CHECK_FALSE (bus.hasUnsavedChanges());

    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok); // before #1
    CHECK (bus.hasUnsavedChanges());

    REQUIRE (bus.redo (lee).ok);
    CHECK_FALSE (bus.hasUnsavedChanges());

    REQUIRE (bus.redo (lee).ok);
    CHECK (bus.hasUnsavedChanges());

    SUBCASE ("after a new change, undoing it gets back to the saved state")
    {
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok); // back to #1, as saved
        REQUIRE (bus.execute (SetTempo { 150 }, lee).ok);  // #3, which drops #2
        CHECK (bus.hasUnsavedChanges());
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        CHECK_FALSE (bus.hasUnsavedChanges());
    }

    SUBCASE ("once a new change drops the saved entry, nothing gets back to it, even the same change again")
    {
        REQUIRE (bus.save ({}, lee).ok);                       // at #2
        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok); // #1
        REQUIRE (bus.execute (SetTempo { 140 }, lee).ok);  // #3, as #2 was, but the history can't tell
        CHECK (bus.hasUnsavedChanges());
    }
}

TEST_CASE ("with no history, the start counts as saved again")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    REQUIRE (bus.execute (SetTempo { 130 }, lee).ok);
    CHECK (bus.hasUnsavedChanges());
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK_FALSE (bus.hasUnsavedChanges());
}

TEST_CASE ("a bus whose start isn't what's on disk counts as unsaved, through undo, until a save")
{
    Fixture fixture;
    auto& bus = fixture.bus;
    bus.markUnsaved(); // started from a recovery copy, say, since the folder didn't load

    CHECK (bus.hasUnsavedChanges());
    REQUIRE (bus.execute (SetTempo { 130 }, lee).ok);
    REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (bus.hasUnsavedChanges());

    REQUIRE (bus.save ({}, lee).ok);
    CHECK_FALSE (bus.hasUnsavedChanges());
    REQUIRE (bus.redo (lee).ok);
    CHECK (bus.hasUnsavedChanges());
}

TEST_CASE ("a command that changes nothing leaves nothing unsaved")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    REQUIRE (bus.execute (SetTempo { 120 }, lee).ok);
    CHECK (bus.history().empty());
    CHECK_FALSE (bus.hasUnsavedChanges());
}

TEST_CASE ("a gesture's steps after a save are unsaved, and a gesture that ends where it started changes nothing")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    SUBCASE ("steps merged into the saved entry")
    {
        REQUIRE (bus.execute (SetTempo { 130, "drag" }, lee).ok); // #1
        REQUIRE (bus.save ({}, lee).ok);
        REQUIRE (bus.execute (SetTempo { 140, "drag" }, lee).ok); // still #1
        REQUIRE (bus.history().size() == 1);
        CHECK (bus.hasUnsavedChanges());

        REQUIRE (bus.save ({}, lee).ok);
        REQUIRE (bus.execute (SetTempo { 130, "drag" }, lee).ok);
        CHECK (bus.hasUnsavedChanges()); // the same value as the first save, but the history can't tell
    }

    SUBCASE ("a gesture back where it started, from a save")
    {
        REQUIRE (bus.save ({}, lee).ok);
        REQUIRE (bus.execute (SetTempo { 130, "drag" }, lee).ok);
        CHECK (bus.hasUnsavedChanges());
        REQUIRE (bus.execute (SetTempo { 120, "drag" }, lee).ok); // no entry left
        CHECK (bus.history().empty());
        CHECK_FALSE (bus.hasUnsavedChanges());
    }
}

TEST_CASE ("saving with a transaction open saves its edits so far, which count as saved once they're an entry")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    REQUIRE (bus.beginTransaction (claude, "wrote verse").ok);
    REQUIRE (bus.execute (SetTempo { 130 }, claude).ok);
    CHECK (bus.hasUnsavedChanges());

    // Lee saves while Claude's transaction is open: no entry, no split.
    const auto saved = bus.save ({}, lee);

    REQUIRE (saved.ok);
    CHECK (fixture.storage.saves.back().project.tempo == 130);
    CHECK_FALSE (bus.hasUnsavedChanges());
    CHECK (bus.history().empty());
    REQUIRE (bus.openTransaction());
    CHECK (bus.openTransaction()->part == 1);

    SUBCASE ("committed")
    {
        REQUIRE (bus.commitTransaction (claude).ok);
        CHECK (bus.history().size() == 1);
        CHECK_FALSE (bus.hasUnsavedChanges());
    }

    SUBCASE ("interrupted")
    {
        REQUIRE (bus.interruptTransaction (claude).ok);
        CHECK (bus.history().size() == 1);
        CHECK_FALSE (bus.hasUnsavedChanges());
    }

    SUBCASE ("split by another author's change, which is then undone")
    {
        REQUIRE (bus.execute (SetMix { "trk-bass", "-6dB" }, lee).ok);
        CHECK (bus.history().size() == 2); // part 1, then Lee's
        CHECK (bus.hasUnsavedChanges());

        REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);
        CHECK_FALSE (bus.hasUnsavedChanges());
    }

    SUBCASE ("more edits")
    {
        REQUIRE (bus.execute (SetTempo { 140 }, claude).ok);
        CHECK (bus.hasUnsavedChanges());
    }

    SUBCASE ("rolled back: the saved files hold edits the project no longer has")
    {
        REQUIRE (bus.rollbackTransaction (claude).ok);
        CHECK (bus.project().tempo == 120);
        CHECK (bus.hasUnsavedChanges());
    }
}

TEST_CASE ("a save that fails, or that has nowhere to go, leaves the changes unsaved")
{
    SUBCASE ("the storage fails")
    {
        Fixture fixture;
        fixture.storage.fail = true;
        REQUIRE (fixture.bus.execute (SetTempo { 130 }, lee).ok);

        const auto saved = fixture.bus.save ({}, lee);

        CHECK_FALSE (saved.ok);
        CHECK (saved.message == "didn't save: the disk is full");
        CHECK (fixture.bus.hasUnsavedChanges());
    }

    SUBCASE ("no storage")
    {
        CommandBus bus (makeProject());
        REQUIRE (bus.execute (SetTempo { 130 }, lee).ok);

        const auto saved = bus.save ({}, lee);

        CHECK_FALSE (saved.ok);
        CHECK (saved.message == "can't save: this session has no project folder to save to");
        CHECK (bus.hasUnsavedChanges());
    }

    SUBCASE ("no author")
    {
        Fixture fixture;
        const auto saved = fixture.bus.save ({}, Author {});

        CHECK_FALSE (saved.ok);
        CHECK (testing::contains (saved.message, "saving needs an author"));
        CHECK (fixture.storage.saves.empty());
    }
}

TEST_CASE ("a save neither ends a gesture nor enters the history")
{
    Fixture fixture;
    auto& bus = fixture.bus;

    REQUIRE (bus.execute (SetTempo { 130, "drag" }, lee).ok);
    REQUIRE (bus.save ({}, claude).ok);
    REQUIRE (bus.execute (SetTempo { 140, "drag" }, lee).ok);

    REQUIRE (bus.history().size() == 1);
    CHECK (testing::contains (bus.history()[0].description, "set the tempo to 140"));
}
