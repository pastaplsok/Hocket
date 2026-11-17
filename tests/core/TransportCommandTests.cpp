// The transport commands (SPEC §3.1): seek, the loop region, loop on and
// off, with play and stop. What they check, what they say, and what they ask
// the transport to do; the engine's side is in tests/engine/LiveTransportTests.cpp.

#include "BusTesting.h"

using namespace hocket;
using namespace hocket::commands;
using namespace hocket::testing;

namespace
{
    constexpr notes::Ticks bar = 3840;
    constexpr notes::Ticks beat = 960;

    const Author lee { "Lee" };
    const Author claude { "Claude", "session-1" };

    // One track with one 8-bar clip at 1.1, so the song is 1.1 to 9.1.
    project::Project makeProject()
    {
        project::Project project;
        project::Track bass { "trk-bass", "Bass", project::TrackType::midi, {}, {} };
        project.notesFiles["clips/a.notes"] = parsedNotes ("@length 8bars\n\n1.1 C2 1/4 v100\n");
        bass.clips.push_back ({ "clp-a", "clips/a.notes", 0 });
        project.tracks.push_back (std::move (bass));
        return project;
    }

    struct Fixture
    {
        Fixture() { bus.setTransport (&transport); }
        ~Fixture() { bus.setTransport (nullptr); }

        Result run (const Command& command) { return bus.execute (command, claude); }
        const TransportSettings& settings() const { return bus.transportSettings(); }

        CommandBus bus { makeProject() };
        FakeTransport transport;
    };
}

TEST_CASE ("transport: what plays, where play starts, and where a playhead outside wraps to")
{
    const auto project = makeProject();
    TransportSettings settings;

    CHECK (playRange (project, settings) == TickRange { 0, 8 * bar });
    CHECK (startPosition (project, settings) == 0);

    settings.seekPosition = 3 * bar;
    CHECK (startPosition (project, settings) == 3 * bar);

    // The region decides, and play starts at its start unless the seek is in it.
    settings.loopRegion = TickRange { 4 * bar, 6 * bar };
    CHECK (playRange (project, settings) == TickRange { 4 * bar, 6 * bar });
    CHECK (startPosition (project, settings) == 4 * bar);
    settings.seekPosition = 5 * bar;
    CHECK (startPosition (project, settings) == 5 * bar);

    // A seek past the song's end, once the song has shrunk, starts at 1.1.
    settings = { {}, true, 12 * bar };
    CHECK (startPosition (project, settings) == 0);

    // As if it had always looped the range: before it and after it, by
    // remainder from its start, so whole bars keep the place in the bar.
    const TickRange range { 10, 20 };
    CHECK (wrapInto (12, range) == 12);
    CHECK (wrapInto (20, range) == 10);
    CHECK (wrapInto (25, range) == 15);
    CHECK (wrapInto (5, range) == 15);
    CHECK (wrapInto (-11, range) == 19);
    CHECK (wrapInto (3, TickRange { 10, 10 }) == 10);
    CHECK (wrapInto (2 * bar + beat, TickRange { 4 * bar, 6 * bar }) == 4 * bar + beat);
}

TEST_CASE ("transport: play says what it plays, from where, and how")
{
    Fixture f;

    CHECK (f.run (Play {}).message == "playing 1.1 to 9.1 on a loop");
    CHECK (f.run (Play {}).message == "already playing");
    CHECK (f.run (Stop {}).message == "stopped");
    CHECK (f.run (Stop {}).message == "already stopped");
    CHECK (f.transport.take() == std::vector<std::string> { "play", "stop" });

    f.run (Seek { "3.1" });
    CHECK (f.run (Play {}).message == "playing 1.1 to 9.1 on a loop, starting at 3.1");
    f.run (Stop {});

    f.run (SetLoopRegion { "5.1", "7.1" });
    CHECK (f.run (Play {}).message == "playing the loop region 5.1 to 7.1 on a loop");
    f.run (Stop {});

    f.run (SetLooping { false });
    f.run (Seek { "6.1" });
    CHECK (f.run (Play {}).message == "playing the loop region 5.1 to 7.1 once (loop is off), starting at 6.1");
}

TEST_CASE ("transport: seek while stopped sets where play starts")
{
    Fixture f;

    const auto result = f.run (Seek { "5.1" });
    CHECK (result.ok);
    CHECK (result.message == "seeked to 5.1, from 1.1; play starts there");
    CHECK (f.settings().seekPosition == 4 * bar);
    CHECK (f.transport.take().empty()); // nothing to jump

    // Play starts there each time, until the next seek.
    f.run (Play {});
    f.run (Stop {});
    CHECK (startPosition (f.bus.project(), f.settings()) == 4 * bar);

    CHECK (f.run (Seek { "5.1" }).message == "play already starts at 5.1");
    CHECK (f.run (Seek { "2.3.2" }).message == "seeked to 2.3.2, from 5.1; play starts there");
    CHECK (f.bus.history().empty());
}

TEST_CASE ("transport: seek while playing jumps, and says where the playhead was")
{
    Fixture f;
    f.run (Play {});
    f.transport.take();

    // To the sixteenth below: the playhead has moved on by the time it's read.
    f.transport.playhead = 5 * bar + 2 * beat + 100;
    CHECK (f.run (Seek { "2.1" }).message == "seeked to 2.1, from 6.3");
    CHECK (f.transport.take() == std::vector<std::string> { "jump to 3840" });
    CHECK (f.settings().seekPosition == bar);
}

TEST_CASE ("transport: seek past the end of the song is refused, unless it's in the loop region")
{
    Fixture f;

    for (const auto* position : { "9.1", "12.1" })
    {
        CAPTURE (position);
        const auto result = f.run (Seek { position });
        CHECK_FALSE (result.ok);
        CHECK (result.message == "seek: " + std::string (position) + " isn't in the song, which runs from 1.1 up to 9.1; seek to a position before 9.1");
    }

    const auto badText = f.run (Seek { "x" });
    CHECK_FALSE (badText.ok);
    CHECK (contains (badText.message, "seek: 'x' isn't a valid position"));
    CHECK (contains (badText.message, "Positions are bar.beat[.sixteenth][+ticks], e.g. '1.1'"));
    CHECK (f.run (Seek { "3.5" }).message
           == "seek: '3.5' isn't a valid position: beat 5 doesn't exist in 4/4 (beats are 1-4). Positions are bar.beat[.sixteenth][+ticks], "
              "e.g. '1.1', '2.3.2', '1.1+30'");

    // A region may run past the song, and a seek into it is fine.
    f.run (SetLoopRegion { "9.1", "11.1" });
    CHECK (f.run (Seek { "10.1" }).message == "seeked to 10.1, from 9.1; play starts there");
    CHECK (f.run (Seek { "12.1" }).message
           == "seek: 12.1 isn't in the song, which runs from 1.1 up to 9.1; seek to a position before 9.1, or in the loop region 9.1 to 11.1");
    CHECK (f.settings().seekPosition == 9 * bar);
}

TEST_CASE ("transport: a seek outside the loop region wraps into it while playing, and waits while stopped")
{
    Fixture f;
    f.run (SetLoopRegion { "3.1", "5.1" });

    CHECK (f.run (Seek { "4.1" }).message == "seeked to 4.1, from 3.1; play starts there");
    CHECK (f.run (Seek { "7.1" }).message == "seeked to 7.1, from 4.1; that's outside the loop region 3.1 to 5.1, so play starts at its start, 3.1");
    CHECK (startPosition (f.bus.project(), f.settings()) == 2 * bar);

    f.run (Play {});
    f.transport.take();
    f.transport.playhead = 3 * bar + beat;

    // 7.3 is two loops of two bars past 3.3.
    CHECK (f.run (Seek { "7.3" }).message == "seeked to 7.3, from 4.2; that's outside the loop region 3.1 to 5.1, so the playhead wrapped into it, to 3.3");
    CHECK (f.transport.take() == std::vector<std::string> { "jump to " + std::to_string (2 * bar + 2 * beat) });
}

TEST_CASE ("transport: setting the loop region says what it was, and where play starts")
{
    Fixture f;
    f.run (Seek { "5.1" });

    // 5.1 is the region's end, so it isn't in it.
    const auto result = f.run (SetLoopRegion { "3.1", "5.1" });
    CHECK (result.ok);
    CHECK (result.message == "loop region 3.1 to 5.1 (2 bars), was the whole song; play starts at 3.1");
    CHECK (f.settings().loopRegion == TickRange { 2 * bar, 4 * bar });
    CHECK (f.transport.take() == std::vector<std::string> { "loop changed" });

    CHECK (f.run (SetLoopRegion { "3.1", "5.1" }).message == "the loop region is already 3.1 to 5.1, so nothing changed");
    CHECK (f.transport.take().empty());

    CHECK (f.run (SetLoopRegion { "1.1", "2.1" }).message == "loop region 1.1 to 2.1 (1 bar), was 3.1 to 5.1; play starts at 1.1");
    CHECK (f.run (SetLoopRegion { "3.1", "4.3" }).message == "loop region 3.1 to 4.3, was 1.1 to 2.1; play starts at 3.1");
    CHECK (f.run (SetLoopRegion { "3.1", "7.1" }).message == "loop region 3.1 to 7.1 (4 bars), was 3.1 to 4.3; play starts at 5.1");

    // Past the song's last clip.
    CHECK (f.run (SetLoopRegion { "7.1", "11.1" }).message
           == "loop region 7.1 to 11.1 (4 bars), was 3.1 to 7.1; the song ends at 9.1, inside it, so the rest is silent until clips are added there; "
              "play starts at 7.1");
    CHECK (f.run (SetLoopRegion { "9.1", "11.1" }).message
           == "loop region 9.1 to 11.1 (2 bars), was 7.1 to 11.1; the song ends at 9.1, before it starts, so it's silent until clips are added there; "
              "play starts at 9.1");
    CHECK (f.bus.history().empty());
}

TEST_CASE ("transport: a loop region set while playing wraps a playhead outside it")
{
    Fixture f;
    f.run (Play {});
    f.transport.take();
    f.transport.playhead = 2 * bar + 2 * beat; // 3.3

    CHECK (f.run (SetLoopRegion { "3.1", "5.1" }).message == "loop region 3.1 to 5.1 (2 bars), was the whole song");
    CHECK (f.run (SetLoopRegion { "9.1", "11.1" }).message
           == "loop region 9.1 to 11.1 (2 bars), was 3.1 to 5.1; the song ends at 9.1, before it starts, so it's silent until clips are added there; "
              "the playhead was at 3.3, outside the loop region, so it wrapped to 9.3");
    CHECK (f.transport.take() == std::vector<std::string> { "loop changed", "loop changed" });

    // Clearing it, the playhead past the song's end wraps into the song.
    f.transport.playhead = 9 * bar;
    CHECK (f.run (ClearLoopRegion {}).message
           == "loop region cleared, was 9.1 to 11.1: play plays the whole song, 1.1 to 9.1; the playhead was at 10.1, outside the song, so it wrapped to 2.1");
}

TEST_CASE ("transport: a loop region must run forwards, within the 999-bar song, for at least a sixteenth")
{
    Fixture f;

    const auto refused = [&f] (const char* start, const char* end)
    {
        const auto result = f.run (SetLoopRegion { start, end });
        CHECK_FALSE (result.ok);
        return result.message;
    };

    CHECK (refused ("5.1", "3.1") == "set the loop region: it would end at 3.1, which isn't after its start, 5.1; the end comes after the start, e.g. 5.1 to 9.1");
    CHECK (refused ("5.1", "5.1") == "set the loop region: it would end at 5.1, which isn't after its start, 5.1; the end comes after the start, e.g. 5.1 to 9.1");
    CHECK (refused ("998.1", "1001.1") == "set the loop region: it would end at 1001.1, but a song is at most 999 bars long, so it ends by 1000.1");
    CHECK (refused ("5.1", "5.1+100") == "set the loop region: 5.1 to 5.1+100 is shorter than a sixteenth note, the shortest loop region");
    CHECK (contains (refused ("5.x", "6.1"), "set the loop region: its start, '5.x' isn't a valid position"));
    CHECK (contains (refused ("5.1", "6.5"), "set the loop region: its end, '6.5' isn't a valid position: beat 5 doesn't exist in 4/4"));

    CHECK (f.run (SetLoopRegion { "999.1", "1000.1" }).ok);
    CHECK (f.run (SetLoopRegion { "5.1", "5.1.2" }).ok);
    CHECK (f.transport.take() == std::vector<std::string> { "loop changed", "loop changed" });
}

TEST_CASE ("transport: clearing the loop region")
{
    Fixture f;
    CHECK (f.run (ClearLoopRegion {}).message == "there's no loop region, so nothing changed: play plays the whole song, 1.1 to 9.1");

    f.run (Seek { "2.1" });
    f.run (SetLoopRegion { "5.1", "7.1" });
    f.transport.take();

    // Play starts at the last seek again.
    CHECK (f.run (ClearLoopRegion {}).message == "loop region cleared, was 5.1 to 7.1: play plays the whole song, 1.1 to 9.1; play starts at 2.1");
    CHECK_FALSE (f.settings().loopRegion);
    CHECK (f.transport.take() == std::vector<std::string> { "loop changed" });
}

TEST_CASE ("transport: loop on and off")
{
    Fixture f;
    CHECK (f.run (SetLooping { true }).message == "loop is already on, so nothing changed");
    CHECK (f.transport.take().empty());

    CHECK (f.run (SetLooping { false }).message == "loop off, was on: play stops at the end of the song, 9.1, instead of looping");
    CHECK_FALSE (f.settings().looping);
    CHECK (f.transport.take() == std::vector<std::string> { "loop changed" });
    CHECK (f.run (SetLooping { false }).message == "loop is already off, so nothing changed");

    CHECK (f.run (Play {}).message == "playing 1.1 to 9.1 once (loop is off)");
    CHECK (f.run (SetLooping { true }).message == "loop on, was off: play loops the whole song, 1.1 to 9.1");

    f.run (SetLoopRegion { "3.1", "5.1" });
    f.transport.playhead = 3 * bar;
    CHECK (f.run (SetLooping { false }).message == "loop off, was on: play stops at the end of the loop region, 5.1, instead of looping");
    CHECK (f.run (SetLooping { true }).message == "loop on, was off: play loops the loop region 3.1 to 5.1");
}

TEST_CASE ("transport: the settings are in ticks, so a time signature or tempo leaves them in place in time")
{
    Fixture f;
    f.run (SetLoopRegion { "3.1", "5.1" });
    f.run (Seek { "4.1" });
    const auto settings = f.settings();

    REQUIRE (f.bus.execute (SetTimeSignature { "2/4" }, lee).ok);
    REQUIRE (f.bus.execute (SetTempo { 90 }, lee).ok);
    CHECK (f.settings() == settings);

    // Written in the new bars.
    CHECK (f.run (Play {}).message == "playing the loop region 5.1 to 9.1 on a loop, starting at 7.1");

    // Undo doesn't touch them.
    REQUIRE (f.bus.undo (lee, UndoScope::anyAuthor).ok);
    CHECK (f.settings() == settings);
}
