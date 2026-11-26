// Live playback through the fake audio device while transport commands
// change what plays (Milestone 1's step 6, part 4): seek, the loop region,
// loop on and off; and a loop region played once, whose end the Edit's clips
// end at while it plays (Lee's decision, 2026-11-17). What the commands check
// and say is in tests/core/TransportCommandTests.cpp.

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

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
    constexpr int a3 = 57;
    constexpr int b3 = 59;

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
        return "C3: " + historyOf (c3, events) + "; E3: " + historyOf (e3, events) + "; G3: " + historyOf (g3, events) + "; A3: "
             + historyOf (a3, events) + "; B3: " + historyOf (b3, events);
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
    // only: the playhead goes to 3.3, where E3 is sounding but isn't struck
    // (SPEC §3.3), and loops back to 3.1 two beats later, where it is.
    const auto changeAt = 2 * quarter + FakeAudioDevice::blockSize;
    device.play (changeAt);
    const auto result = run (bus, sync, commands::SetLoopRegion { "3.1", "4.1" });
    CHECK (result.message == "loop region 3.1 to 4.1 (1 bar), was the whole song; the playhead was at 1.3, outside the loop region, so it wrapped to 3.3");
    letEditSettle (device, sync);
    device.play (2 * quarter + 2 * barSamples - 3 * FakeAudioDevice::blockSize);

    const auto events = recorder.recorded();
    INFO (history (events));
    CHECK (onceNear (times (events, c3, false), changeAt));
    CHECK (times (events, e3, true, 0, changeAt + quarter).empty());
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

//==============================================================================
// A loop region played once (loop off). More of the song follows its end, so
// while it plays the Edit's clips end there (Lee's decision, 2026-11-17): the
// notes sounding end on time and ring out, and nothing after it starts before
// the stop, which a message-thread timer makes a little late. Every way out of
// playing once gives the clips their own ends back. checkEditMatches works out
// from the bus what the Edit should have at each point.
namespace
{
    // The region is bar 3. The bass holds C3 through bar 1, and has a two-bar
    // clip from 3.1 whose E3 runs from 3.3 across the region's end to 4.3,
    // with G3 at 4.1; the lead plays A3 at 4.1. Played once, the bass's clip
    // ends at 4.1 and the lead's isn't in the Edit, so E3 ends at 4.1 and
    // neither G3 nor A3 is struck.
    project::Project playOnceProject()
    {
        return makeProject ({ makeTrack ("trk-bass", "Bass",
                                         {
                                             makeClip ("clp-one", 0, "@length 1bar\n\n1.1 C3 1/1 v100\n"),
                                             makeClip ("clp-across", 2 * bar, "@length 2bars\n\n1.3 E3 1/1 v100\n2.1 G3 1/8 v100\n"),
                                         }),
                              makeTrack ("trk-lead", "Lead", { makeClip ("clp-after", 3 * bar, "@length 1bar\n\n1.1 A3 1/8 v100\n") }) });
    }

    // Bar 3 playing once, with a recorder on each track, whose sample 0 is 3.1.
    struct PlayingOnce
    {
        FakeAudioDevice device;
        commands::CommandBus bus { playOnceProject() };
        engine::EditSync sync { testEngine(), bus };
        MidiRecorder& bass = MidiRecorder::addTo (sync.edit(), 0);
        MidiRecorder& lead = MidiRecorder::addTo (sync.edit(), 1);

        PlayingOnce()
        {
            run (bus, sync, commands::SetLoopRegion { "3.1", "4.1" });
            run (bus, sync, commands::SetLooping { false });
            checkEditMatches (sync, bus); // stopped: the clips are whole
            REQUIRE (leadClips() == 1);

            CHECK (timedPlay (bus, testing::lee).message == "playing the loop region 3.1 to 4.1 once (loop is off)");
            checkEditMatches (sync, bus); // they end at 4.1
            CHECK (leadClips() == 0);
        }

        // Not counting those on their way out, emptied (EditSync::endingClips).
        int leadClips()
        {
            const auto clips = tracktion::getAudioTracks (sync.edit())[1]->getClips();
            return static_cast<int> (std::ranges::count_if (clips, [] (tracktion::Clip* clip) { return ! clip->getName().endsWith (" (ending)"); }));
        }

        // Samples played since play started.
        int played() const { return device.output().buffer.getNumSamples(); }

        // Sends a command, and checks the Edit after it.
        commands::Result send (const commands::Command& command)
        {
            auto result = run (bus, sync, command);
            checkEditMatches (sync, bus);
            return result;
        }
    };

    // The notes struck at or after this sample.
    std::vector<MidiRecorder::Event> struckFrom (const std::vector<MidiRecorder::Event>& events, std::int64_t from)
    {
        std::vector<MidiRecorder::Event> struck;
        std::ranges::copy_if (events, std::back_inserter (struck), [from] (const MidiRecorder::Event& e) { return e.note >= 0 && e.on && e.sample >= from; });
        return struck;
    }
}

TEST_CASE ("live: a loop region played once ends the notes sounding at its end, which ring out, strikes none after it, and its clips are whole when it's done")
{
    PlayingOnce once;

    // Eight blocks past the end before the stop, as when the timer is late:
    // the message loop doesn't run until after them.
    once.device.play (barSamples + 8 * FakeAudioDevice::blockSize);
    checkEditMatches (once.sync, once.bus); // still playing once, so still cut short
    CHECK (once.leadClips() == 0);
    runMessages (20);
    CHECK (once.bus.isPlaying() == false);
    CHECK_FALSE (once.sync.stoppedByItself());
    checkEditMatches (once.sync, once.bus); // whole again
    CHECK (once.leadClips() == 1);

    // The Edit plays on while stopped, so what's sounding rings out.
    const auto stoppedAt = once.played();
    once.device.play (2 * quarter);

    const auto bass = once.bass.recorded();
    INFO (history (bass));
    CHECK (onceNear (times (bass, e3, true), 2 * quarter));
    CHECK (onceNear (times (bass, e3, false), barSamples));
    CHECK (struckFrom (bass, barSamples).empty());
    CHECK (struckFrom (once.lead.recorded(), 0).empty());

    // E3 sounds on after its note-off, through the stop with no sudden drop,
    // and dies away.
    constexpr auto block = FakeAudioDevice::blockSize;
    const auto& out = once.device.output().buffer;
    const auto beforeStop = peakBetween (out, stoppedAt - block, stoppedAt);
    const auto afterStop = peakBetween (out, stoppedAt, stoppedAt + block);
    CAPTURE (beforeStop);
    CAPTURE (afterStop);
    CHECK (peakBetween (out, barSamples, barSamples + block) > 0.01f);
    CHECK (beforeStop > 1.0e-3f);
    CHECK (afterStop > 0.5f * beforeStop);
    CHECK (peakBetween (out, once.played() - block, once.played()) < 1.0e-3f);
}

TEST_CASE ("live: stop while a loop region plays once makes its clips whole again, and the next play cuts them short again")
{
    PlayingOnce once;
    once.device.play (quarter);
    once.send (commands::Stop {});
    CHECK (once.leadClips() == 1);

    REQUIRE (timedPlay (once.bus, testing::lee).ok);
    checkEditMatches (once.sync, once.bus);
    CHECK (once.leadClips() == 0);

    once.device.play (quarter);
    once.send (commands::Stop {});
    CHECK (once.leadClips() == 1);
}

TEST_CASE ("live: loop on while a loop region plays once makes its clips whole again, and the region loops")
{
    PlayingOnce once;
    once.device.play (3 * quarter);
    once.send (commands::SetLooping { true });
    CHECK (once.leadClips() == 1);

    // On past the next pass's E3, at 3.3 again.
    once.device.play (barSamples);
    runMessages (20);
    CHECK (once.bus.isPlaying() == true);
    checkEditMatches (once.sync, once.bus);

    const auto bass = once.bass.recorded();
    INFO (history (bass));
    CHECK (onceNear (times (bass, e3, true, 3 * quarter), barSamples + 2 * quarter));
    CHECK (times (bass, g3, true).empty());
    CHECK (struckFrom (once.lead.recorded(), 0).empty());
}

TEST_CASE ("live: a new loop region while one plays once moves where the clips end, and a playhead outside it wraps in, with loop off too")
{
    PlayingOnce once;

    // Early in bar 3, a region on to 5.1: the bass's clip is whole, and the lead's is back.
    once.device.play (quarter / 2);
    CHECK (once.send (commands::SetLoopRegion { "3.1", "5.1" }).message == "loop region 3.1 to 5.1 (2 bars), was 3.1 to 4.1");
    CHECK (once.leadClips() == 1);

    // Just past 3.2, a region of bar 1: the playhead wraps to just past 1.2,
    // where C3 is sounding but isn't struck (SPEC §3.3), and plays on to
    // 2.1, once, so C3 never sounds. Its place in the bar is kept, so 2.1
    // comes where 4.1 would have. Only bar 1's clip is left in the Edit.
    once.device.play (quarter / 2 + FakeAudioDevice::blockSize);
    CHECK (once.send (commands::SetLoopRegion { "1.1", "2.1" }).message
           == "loop region 1.1 to 2.1 (1 bar), was 3.1 to 5.1; the playhead was at 3.2, outside the loop region, so it wrapped to 1.2");
    CHECK (once.leadClips() == 0);

    once.device.play (3 * quarter + 8 * FakeAudioDevice::blockSize);
    checkEditMatches (once.sync, once.bus);
    runMessages (20);
    CHECK (once.bus.isPlaying() == false);
    checkEditMatches (once.sync, once.bus);
    CHECK (once.leadClips() == 1);

    const auto bass = once.bass.recorded();
    INFO (history (bass));
    CHECK (times (bass, c3, true).empty());
    CHECK (times (bass, e3, true).empty());
    CHECK (struckFrom (bass, barSamples).empty());
    CHECK (struckFrom (once.lead.recorded(), 0).empty());
}

TEST_CASE ("live: clearing the loop region while it plays once makes its clips whole, and the song plays on to its end, once")
{
    PlayingOnce once;
    once.device.play (quarter);
    CHECK (once.send (commands::ClearLoopRegion {}).message == "loop region cleared, was 3.1 to 4.1: play plays the whole song, 1.1 to 5.1");
    CHECK (once.leadClips() == 1);

    // E3 carries on across 4.1, where G3 and A3 are struck, and the song
    // ends at 5.1.
    once.device.play (2 * barSamples - quarter + 8 * FakeAudioDevice::blockSize);
    runMessages (20);
    CHECK (once.bus.isPlaying() == false);
    checkEditMatches (once.sync, once.bus);

    const auto bass = once.bass.recorded();
    const auto lead = once.lead.recorded();
    INFO (history (bass));
    CHECK (onceNear (times (bass, e3, false), barSamples + 2 * quarter));
    CHECK (onceNear (times (bass, g3, true), barSamples));
    CHECK (onceNear (times (lead, a3, true), barSamples));
}

TEST_CASE ("live: clips written, moved and added while a loop region plays once end at its end too, and have those changes when they're whole again")
{
    PlayingOnce once;
    once.device.play (quarter);

    // A bar longer, with B3 for G3 after the end: still cut short at 4.1.
    once.send (commands::WriteClip { "clp-across", "@length 3bars\n\n1.3 E3 1/1 v100\n2.1 B3 1/8 v100\n" });

    // The lead's clip into the region, behind the playhead: it's in the Edit,
    // and its A3 waits for the next pass.
    once.send (commands::MoveClip { "clp-after", "3.1", {} });
    CHECK (once.leadClips() == 1);

    // Bar 1's clip past the end, and a clip added at it: neither is in the Edit.
    once.send (commands::MoveClip { "clp-one", "6.1", {} });
    once.send (commands::AddClip { "trk-lead", "4.1", commands::ClipText { "@length 1bar\n\n1.1 B3 1/8 v100\n" }, "clp-new", {} });
    CHECK (once.leadClips() == 1);

    // And the clip added is taken back.
    REQUIRE (once.bus.undo (testing::lee, commands::UndoScope::anyAuthor).ok);
    once.sync.edit().dispatchPendingUpdatesSynchronously();
    checkEditMatches (once.sync, once.bus);

    once.device.play (3 * quarter + 8 * FakeAudioDevice::blockSize);
    checkEditMatches (once.sync, once.bus);
    runMessages (20);
    CHECK (once.bus.isPlaying() == false);
    checkEditMatches (once.sync, once.bus); // whole, with the changes
    CHECK (once.leadClips() == 1);

    const auto bass = once.bass.recorded();
    INFO (history (bass));
    CHECK (onceNear (times (bass, e3, true), 2 * quarter));
    CHECK (onceNear (times (bass, e3, false), barSamples));
    CHECK (struckFrom (bass, barSamples).empty());
    CHECK (struckFrom (once.lead.recorded(), 0).empty());
}

TEST_CASE ("live: when Tracktion stops by itself while a loop region plays once, its clips are whole again")
{
    PlayingOnce once;
    once.device.play (quarter);

    // As when Tracktion rebuilds its device lists.
    once.sync.edit().getTransport().stop (false, false);
    CHECK (once.sync.stoppedByItself());
    runMessages (20);
    checkEditMatches (once.sync, once.bus);
    CHECK (once.leadClips() == 1);
}
