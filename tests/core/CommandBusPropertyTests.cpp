// Random sequences of edits, undos and redos, checked against a reference
// model that keeps a full copy of the project for every history step.

#include "core/commands/CommandBus.h"

#include <doctest/doctest.h>

#include <random>

using namespace hocket;
using namespace hocket::commands;

namespace
{
    using Rng = std::mt19937;

    int randomInt (Rng& rng, int low, int high)
    {
        return std::uniform_int_distribution<int> (low, high) (rng);
    }

    project::Project makeProject()
    {
        project::Track track { "trk-bass", "Bass", project::TrackType::midi, { { "dev-bass-synth", project::DeviceType::fourOsc } }, {} };
        track.clips.push_back ({ "clp-a", "clips/a.notes", 0, {} });
        track.clips.push_back ({ "clp-b", "clips/b.notes", 4 * 3840, {} });

        project::Project project;
        project.tracks.push_back (std::move (track));
        return project;
    }

    // Small clips, so writes sometimes repeat the current text (a no-op).
    notes::Clip randomClip (Rng& rng, int maxBars)
    {
        notes::Clip clip;
        clip.lengthInBars = randomInt (rng, 1, maxBars);

        for (int i = randomInt (rng, 0, 2); i > 0; --i)
        {
            notes::Note note;
            note.start = randomInt (rng, 0, 3) * 960 + i; // distinct per note
            note.pitch = randomInt (rng, 36, 38);
            clip.notes.push_back (note);
        }

        return clip;
    }
}

TEST_CASE ("undo and redo always match a reference history of whole-project copies")
{
    int changes = 0;
    int noChanges = 0;
    int undos = 0;
    int redos = 0;

    for (std::uint32_t seed = 1; seed <= 300; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        CommandBus bus (makeProject());

        auto expected = bus.project();
        std::vector<project::Project> undoStates; // the project before each history entry
        std::vector<project::Project> redoStates;

        for (int step = 0; step < 40; ++step)
        {
            CAPTURE (step);

            switch (randomInt (rng, 0, 3))
            {
                case 0:
                case 1:
                {
                    const auto clipIndex = static_cast<std::size_t> (randomInt (rng, 0, 1));
                    const auto clipId = clipIndex == 0 ? "clp-a" : "clp-b";
                    const auto text = notes::write (randomClip (rng, clipIndex == 0 ? 4 : 3)); // clp-a must stay clear of clp-b
                    const auto author = randomInt (rng, 0, 1) == 0 ? Author { "Lee" } : Author { "Claude", "session-1" };

                    const auto result = bus.execute (WriteClip { clipId, text }, author);
                    REQUIRE_MESSAGE (result.ok, result.message);

                    // The bus stores what the text parses to: sorted, with spellings filled in.
                    auto next = expected;
                    next.tracks[0].clips[clipIndex].content = *notes::parse (text).clip;

                    if (next != expected)
                    {
                        ++changes;
                        undoStates.push_back (expected);
                        redoStates.clear();
                        expected = std::move (next);
                    }
                    else
                    {
                        ++noChanges;
                    }

                    break;
                }

                case 2:
                    CHECK (bus.undo (Author { "Lee" }, UndoScope::anyAuthor).ok == ! undoStates.empty());

                    if (! undoStates.empty())
                    {
                        ++undos;
                        redoStates.push_back (expected);
                        expected = undoStates.back();
                        undoStates.pop_back();
                    }

                    break;

                default:
                    CHECK (bus.redo (Author { "Lee" }).ok == ! redoStates.empty());

                    if (! redoStates.empty())
                    {
                        ++redos;
                        undoStates.push_back (expected);
                        expected = redoStates.back();
                        redoStates.pop_back();
                    }

                    break;
            }

            REQUIRE (bus.project() == expected);
            REQUIRE (bus.history().size() == undoStates.size());
        }
    }

    // Every path must get real exercise, or the test proves little.
    MESSAGE ("changes: " << changes << ", no-op writes: " << noChanges << ", undos: " << undos << ", redos: " << redos);
    CHECK (changes >= 500);
    CHECK (noChanges >= 100);
    CHECK (undos >= 500);
    CHECK (redos >= 200);
}
