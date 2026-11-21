// What the transport reports about playback (Milestone 1's step 10): how much
// has played since play started, and a wrap or stop that a change to the
// project caused, said in that change's result (SPEC §6.4).

#include "EngineTesting.h"
#include "core/notes/NotesSyntax.h"

#include <doctest/doctest.h>

#include <cstdlib>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // At 120 BPM and 44.1 kHz.
    constexpr int beatSamples = 22050;
    constexpr int barSamples = 4 * beatSamples;

    // A block's worth of ticks at 120 BPM, and a little more.
    constexpr notes::Ticks blockTicks = 12;

    const std::string shortNote = "@length 1bar\n\n1.1 C3 1/8 v100\n";

    // The bass's clip at 1.1 and the lead's at 3.1, so the song is three bars.
    project::Project threeBars()
    {
        return makeProject ({
            makeTrack ("trk-bass", "Bass", { makeClip ("clp-a", 0, shortNote) }),
            makeTrack ("trk-lead", "Lead", { makeClip ("clp-later", 2 * bar, shortNote) }),
        });
    }

    commands::Result run (commands::CommandBus& bus, engine::EditSync& sync, const commands::Command& command)
    {
        auto result = bus.execute (command, testing::claude);
        sync.edit().dispatchPendingUpdatesSynchronously();
        return result;
    }

    bool contains (const std::string& text, std::string_view part)
    {
        return text.find (part) != std::string::npos;
    }

    commands::PlayProgress progressOf (const commands::CommandBus& bus)
    {
        REQUIRE (bus.currentTransport() != nullptr);
        return bus.currentTransport()->playProgress();
    }
}

TEST_CASE ("live: a change that ends the song before the playhead says the playback wrapped, and where")
{
    FakeAudioDevice device;
    commands::CommandBus bus (threeBars());
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // Near 2.2 (the fake device plays whole blocks), the song shrinks to a bar.
    device.play (barSamples + beatSamples);
    const auto place = bus.currentTransport()->playheadPosition();
    const auto result = run (bus, sync, commands::RemoveClip { "clp-later" });
    CHECK (result.message == "#1 removed clip clp-later from trk-lead. The song now ends at 2.1, before where the playhead was, so playback wrapped from "
                                 + notes::formatPosition (place, {}) + " to " + notes::formatPosition (place - bar, {}));

    // Undo brings the song's end back past the playhead, so nothing moves.
    const auto undone = bus.undo (testing::claude, commands::UndoScope::callersOwn);
    CHECK (undone.message == "undid #1 \"removed clip clp-later from trk-lead\" by Claude");

    // A seek's wrap is in its own result, and no later change reports it.
    REQUIRE (bus.execute (commands::SetLoopRegion { "1.1", "2.1" }, testing::lee).ok);
    CHECK (run (bus, sync, commands::SetTempo { 100.0 }).message == "#2 set the tempo to 100");
}

TEST_CASE ("live: with loop off, a change that ends the song before the playhead says playback stops there")
{
    FakeAudioDevice device;
    commands::CommandBus bus (threeBars());
    engine::EditSync sync (testEngine(), bus);
    REQUIRE (bus.execute (commands::SetLooping { false }, testing::lee).ok);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    device.play (barSamples + beatSamples);
    CHECK (run (bus, sync, commands::RemoveClip { "clp-later" }).message
           == "#1 removed clip clp-later from trk-lead. The song now ends at 2.1, before the playhead, and loop is off, so playback stops there");
}

TEST_CASE ("live: how much has played since play started: each pass of the loop counted, a seek's jump not, and from zero at each play")
{
    FakeAudioDevice device;
    commands::CommandBus bus (threeBars());
    engine::EditSync sync (testEngine(), bus);

    CHECK_FALSE (progressOf (bus).started);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // Through the end of the three bars, and on into the next pass.
    device.play (3 * barSamples + 2 * beatSamples);
    auto progress = progressOf (bus);
    CHECK (progress.started);
    CHECK (std::abs (progress.ticks - (3 * bar + 2 * beat)) <= blockTicks);

    // A seek jumps; the jump isn't playing.
    REQUIRE (bus.execute (commands::Seek { "3.1" }, testing::lee).ok);
    device.play (beatSamples);
    progress = progressOf (bus);
    CHECK (std::abs (progress.ticks - (3 * bar + 3 * beat)) <= 2 * blockTicks);

    // Stopped, it holds; played again, it counts from there.
    REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
    const auto held = progressOf (bus);
    device.play (barSamples);
    CHECK (progressOf (bus).ticks == held.ticks);
    CHECK (progressOf (bus).seconds == held.seconds);

    REQUIRE (timedPlay (bus, testing::lee).ok);
    device.play (beatSamples);
    CHECK (std::abs (progressOf (bus).ticks - beat) <= blockTicks);
}
