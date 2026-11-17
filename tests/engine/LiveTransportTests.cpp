// Live playback through the fake audio device while transport commands
// change what plays (Milestone 1's step 6, part 4): seek, the loop region,
// loop on and off. What the commands check and say is in
// tests/core/TransportCommandTests.cpp.

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // At 120 BPM and 44.1 kHz.
    constexpr int quarter = 22050;
    constexpr int barSamples = 4 * quarter;
    constexpr int c3 = 48;
    constexpr int e3 = 52;
    constexpr int g3 = 55;

    // C3 held through bar 1, E3 held through bar 3, a short G3 at 4.1: the
    // song is 1.1 to 5.1, and each bar says where the playhead is.
    project::Project markedProject()
    {
        return makeProject ({ makeTrack ("trk-bass", "Bass",
                                         {
                                             makeClip ("clp-one", 0, "@length 1bar\n\n1.1 C3 1/1 v100\n"),
                                             makeClip ("clp-three", 2 * bar, "@length 1bar\n\n1.1 E3 1/1 v100\n"),
                                             makeClip ("clp-four", 3 * bar, "@length 1bar\n\n1.1 G3 1/8 v100\n"),
                                         }) });
    }

    // What the app does on the message thread straight after a command.
    commands::Result run (commands::CommandBus& bus, engine::EditSync& sync, const commands::Command& command)
    {
        const auto result = bus.execute (command, testing::claude);
        INFO (result.message);
        REQUIRE (result.ok);
        sync.edit().dispatchPendingUpdatesSynchronously();
        return result;
    }

    // The samples where this note starts (on) or ends (off), in [from, to).
    std::vector<std::int64_t> times (const std::vector<MidiRecorder::Event>& events, int note, bool on, std::int64_t from = 0,
                                     std::int64_t to = INT64_MAX)
    {
        std::vector<std::int64_t> found;

        for (const auto& e : events)
            if (e.note == note && e.on == on && e.sample >= from && e.sample < to)
                found.push_back (e.sample);

        return found;
    }

    // Exactly one, within a block of `expected`.
    bool onceNear (const std::vector<std::int64_t>& found, std::int64_t expected)
    {
        return found.size() == 1 && std::abs (found.front() - expected) <= FakeAudioDevice::blockSize;
    }

    // Runs the message loop, as the app does between commands, so timers fire.
    void runMessages (int milliseconds)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
    }

    std::string history (const std::vector<MidiRecorder::Event>& events)
    {
        return "C3: " + historyOf (c3, events) + "; E3: " + historyOf (e3, events) + "; G3: " + historyOf (g3, events);
    }
}

TEST_CASE ("live: seek while playing jumps there, ending the notes sounding and striking those on at the new place")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // At 1.2, to 3.3: E3 is sounding there, and G3 is two beats on.
    device.play (quarter);
    run (bus, sync, commands::Seek { "3.3" });
    device.play (3 * quarter);

    const auto events = recorder.recorded();
    INFO (history (events));
    CHECK (onceNear (times (events, c3, false), quarter));
    CHECK (onceNear (times (events, e3, true), quarter));
    CHECK (onceNear (times (events, g3, true), 3 * quarter));
}

TEST_CASE ("live: a seek while stopped is where play starts")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());

    run (bus, sync, commands::Seek { "3.1" });
    const auto played = timedPlay (bus, testing::lee);
    CHECK (played.message == "playing 1.1 to 5.1 on a loop, starting at 3.1");
    device.play (2 * barSamples + quarter);

    // E3 first, then G3 a bar later, then the loop comes round to 1.1 and C3.
    const auto events = recorder.recorded();
    INFO (history (events));
    CHECK (onceNear (times (events, e3, true), 0));
    CHECK (onceNear (times (events, g3, true), barSamples));
    CHECK (onceNear (times (events, c3, true), 2 * barSamples));
}

TEST_CASE ("live: a loop region set while playing wraps the playhead into it, keeping its place in the bar, and loops it")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // Just past 1.3 (the device plays whole blocks), the region is bar 3
    // only: the playhead goes to 3.3, where E3 is sounding, and loops back
    // to 3.1 two beats later.
    const auto changeAt = 2 * quarter + FakeAudioDevice::blockSize;
    device.play (changeAt);
    const auto result = run (bus, sync, commands::SetLoopRegion { "3.1", "4.1" });
    CHECK (result.message == "loop region 3.1 to 4.1 (1 bar), was the whole song; the playhead was at 1.3, outside the loop region, so it wrapped to 3.3");
    device.play (2 * quarter + 2 * barSamples);

    const auto events = recorder.recorded();
    INFO (history (events));
    CHECK (onceNear (times (events, c3, false), changeAt));
    CHECK (onceNear (times (events, e3, true, 0, changeAt + quarter), changeAt));
    CHECK (onceNear (times (events, e3, true, changeAt + quarter, changeAt + 2 * quarter + barSamples / 2), changeAt + 2 * quarter));
    CHECK (onceNear (times (events, e3, true, changeAt + 2 * quarter + barSamples / 2, changeAt + 2 * quarter + 3 * barSamples / 2),
                     changeAt + 2 * quarter + barSamples));
    CHECK (times (events, g3, true).empty());
    CHECK (sync.loopRange() == sync.edit().getTransport().getLoopRange());
}

TEST_CASE ("live: changing the loop region around the playhead doesn't strike its held note again")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());

    run (bus, sync, commands::SetLoopRegion { "3.1", "5.1" });
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // At 3.2, on a later pass, the region shrinks to bar 3: E3 carries on to
    // the bar's end, then starts again.
    device.play (2 * barSamples + quarter);
    run (bus, sync, commands::SetLoopRegion { "3.1", "4.1" });
    device.play (3 * quarter + quarter);

    const auto changeAt = 2 * barSamples + quarter;
    const auto events = recorder.recorded();
    INFO (history (events));
    CHECK (times (events, e3, true, changeAt - FakeAudioDevice::blockSize, changeAt + 3 * quarter - FakeAudioDevice::blockSize).empty());
    CHECK (onceNear (times (events, e3, true, changeAt + quarter), changeAt + 3 * quarter));
    CHECK (times (events, g3, true, changeAt).empty());
}

TEST_CASE ("live: with loop off, play runs to the end of the song and stops, which isn't a stop by itself")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());

    run (bus, sync, commands::SetLooping { false });
    CHECK (timedPlay (bus, testing::lee).message == "playing 1.1 to 5.1 once (loop is off)");

    // Short of the end, it plays on.
    device.play (4 * barSamples - 2 * FakeAudioDevice::blockSize);
    runMessages (20);
    CHECK (bus.isPlaying() == true);

    // Past it, the next check stops it.
    device.play (4 * FakeAudioDevice::blockSize);
    const auto events = recorder.recorded();
    runMessages (20);
    CHECK (bus.isPlaying() == false);
    CHECK_FALSE (sync.stoppedByItself());

    INFO (history (events));
    CHECK (times (events, c3, true).size() == 1);

    // Play again starts from the start.
    CHECK (timedPlay (bus, testing::lee).message == "playing 1.1 to 5.1 once (loop is off)");
}

TEST_CASE ("live: loop off on a later pass keeps the playhead's place, and the song plays to its end")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // At 3.2 on the second pass: the playhead has counted six bars and a
    // beat, but it's at 3.2, where E3 is sounding.
    const auto changeAt = 4 * barSamples + 2 * barSamples + quarter;
    device.play (changeAt);
    const auto result = run (bus, sync, commands::SetLooping { false });
    CHECK (result.message == "loop off, was on: play stops at the end of the song, 5.1, instead of looping");
    device.play (3 * quarter + barSamples + 4 * FakeAudioDevice::blockSize);
    const auto events = recorder.recorded();
    runMessages (20);

    INFO (history (events));
    CHECK (times (events, e3, true, changeAt - FakeAudioDevice::blockSize).empty());
    CHECK (onceNear (times (events, g3, true, changeAt), changeAt + 3 * quarter));
    CHECK (times (events, c3, true, changeAt).empty());
    CHECK (bus.isPlaying() == false);
    CHECK_FALSE (sync.stoppedByItself());
}

TEST_CASE ("live: loop on again before the end, and it loops")
{
    FakeAudioDevice device;
    commands::CommandBus bus (markedProject());
    engine::EditSync sync (testEngine(), bus);
    auto& recorder = MidiRecorder::addTo (sync.edit());

    run (bus, sync, commands::SetLooping { false });
    REQUIRE (timedPlay (bus, testing::lee).ok);
    device.play (2 * barSamples);
    runMessages (20);
    run (bus, sync, commands::SetLooping { true });
    device.play (2 * barSamples + quarter);
    runMessages (20);

    const auto events = recorder.recorded();
    INFO (history (events));
    CHECK (bus.isPlaying() == true);
    CHECK (onceNear (times (events, c3, true, barSamples), 4 * barSamples));
}
