// Random sequences of every command, invalid ones included (step 4's part 8).
// A refused command changes nothing; after any other, the project writes and
// reads back unchanged, so the bus never makes a project the reader would
// refuse. Undoing everything gives the original, and redoing everything the
// same end state.

#include "BusTesting.h"
#include "common/RandomCommands.h"

#include <map>

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };
}

TEST_CASE ("random command sequences: refusals change nothing, every project reads back, and undo and redo go all the way")
{
    std::map<std::size_t, std::pair<int, int>> outcomes; // by command kind: applied, refused
    int undos = 0;
    int redos = 0;

    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        CAPTURE (seed);
        Rng rng { seed };
        const auto original = randomSequenceProject();
        CommandBus bus (original);

        for (int step = 0; step < 40; ++step)
        {
            CAPTURE (step);
            const auto author = chance (rng, 50) ? lee : claude;

            if (chance (rng, 8))
            {
                undos += bus.undo (author, UndoScope::anyAuthor).ok ? 1 : 0;
                continue;
            }

            if (chance (rng, 7))
            {
                redos += bus.redo (author).ok ? 1 : 0;
                continue;
            }

            const auto command = randomCommand (rng, bus.project());
            const auto before = bus.project();
            const auto history = bus.history();
            const auto result = bus.execute (command, author);
            CAPTURE (result.message);

            if (result.ok)
            {
                ++outcomes[command.index()].first;
                REQUIRE (filesMatchClips (bus.project()));
                checkReadsBack (bus.project());
            }
            else
            {
                ++outcomes[command.index()].second;
                REQUIRE (bus.project() == before);
                REQUIRE (bus.history() == history);
            }
        }

        const auto end = bus.project();
        const auto entries = bus.history().size();

        while (! bus.history().empty())
            REQUIRE (bus.undo (lee, UndoScope::anyAuthor).ok);

        REQUIRE (bus.project() == original);

        for (std::size_t i = 0; i < entries; ++i)
            REQUIRE (bus.redo (lee).ok);

        REQUIRE (bus.project() == end);
    }

    // Every command must get real exercise both ways, or the test proves little.
    for (const auto& [kind, counts] : outcomes)
    {
        CAPTURE (kind);
        MESSAGE ("command " << kind << ": applied " << counts.first << ", refused " << counts.second);
        CHECK (counts.first >= 20);
        CHECK (counts.second >= 20);
    }

    CHECK (outcomes.size() == 14); // every project command (not play and stop)
    MESSAGE ("undos: " << undos << ", redos: " << redos);
    CHECK (undos >= 200);
    CHECK (redos >= 50);
}
