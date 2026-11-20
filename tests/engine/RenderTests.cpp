// Offline renders (SPEC §3.3 sync tests): after a command applies, the Edit
// sounds the way the model says.

#include "EngineTesting.h"

#include "core/Paths.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // Where the first note starts in a render, in seconds; -1 if it's silent.
    // The threshold is 12 dB under firstSoundAt's usual one, as 4OSC is
    // trimmed by 12 dB (2026-11-20), so it finds the same point in the attack.
    double firstNoteAt (engine::EditSync& sync)
    {
        const auto audio = renderLoop (sync);
        const auto sample = firstSoundAt (audio.buffer, 0, 2.5e-4f);
        return sample < 0 ? -1.0 : sample / audio.sampleRate;
    }

    // Within 2 ms: the attack takes a few samples to cross the threshold.
    bool near (double seconds, double expected)
    {
        return std::abs (seconds - expected) < 0.002;
    }
}

TEST_CASE ("the demo project renders: not silent, two bars long, starting on its first note")
{
    auto loaded = project::load (pathFromUtf8 (HOCKET_EXAMPLES_DIR "/m0-demo"));
    REQUIRE (loaded.project);
    commands::CommandBus bus (std::move (*loaded.project));
    engine::EditSync sync (testEngine(), bus);

    const auto audio = renderLoop (sync);

    CHECK (audio.buffer.getNumSamples() == audio.sampleAt (4.0)); // 2 bars at 120 BPM
    CHECK (peakBetween (audio.buffer, 0, audio.buffer.getNumSamples()) > 0.05f);
    CHECK (firstSoundAt (audio.buffer, 0, 2.5e-4f) < audio.sampleAt (0.002)); // as firstNoteAt: 4OSC is trimmed by 12 dB
}

TEST_CASE ("hocket:synth's level: one note at v127 peaks at about -12 dBFS, the bundled kits' kick level; a five-note chord says how loud it gets")
{
    // 4OSC stands in for Hocket's synth until Milestone 4, trimmed so its
    // level sits with the kits' (Lee, 2026-11-20). Played on the fake device,
    // whose float buffers keep a peak over full scale, which a render's file
    // would clip. Its oscillators' phases differ from run to run, so a few runs.
    const auto peakOf = [] (const std::string& line)
    {
        FakeAudioDevice device;
        commands::CommandBus bus (makeProject ({ makeTrack ("trk-keys", "Keys", { makeClip ("clp-keys", 0, "@length 1bar\n\n" + line + "\n") }) }));
        engine::EditSync sync (testEngine(), bus);
        REQUIRE (timedPlay (bus, testing::lee).ok);
        device.play (static_cast<int> (FakeAudioDevice::sampleRate * 1.5));
        const auto& out = device.output().buffer;
        return 20.0 * std::log10 (static_cast<double> (peakBetween (out, 0, out.getNumSamples())));
    };

    for (int run = 0; run < 3; ++run)
    {
        const auto note = peakOf ("1.1 C3 1/1 v127");
        const auto chord = peakOf ("1.1 [C3 E3 G3 C4 E4] 1/1 v127");
        MESSAGE ("one note at v127 peaks at " << note << " dBFS; the chord C3 E3 G3 C4 E4 at " << chord << " dBFS");
        CHECK (std::abs (note - -12.0) < 1.0);
    }
}

TEST_CASE ("notes sound where the clip and the .notes text put them; write_clip and undo move them")
{
    // A clip at 2.1 (2 s in at 120 BPM) with one note at its 1.2 (0.5 s into the clip).
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", bar, "@length 1bar\n\n1.2 C3 1/8 v100\n") }) }));
    engine::EditSync sync (testEngine(), bus);

    const auto original = firstNoteAt (sync);
    CHECK_MESSAGE (near (original, 2.5), "first note at " << original << " s");

    // 1.3.3 is beat 3 plus two sixteenths: 1.25 s into the clip.
    REQUIRE (bus.execute (commands::WriteClip { "clp-bass", "@length 1bar\n\n1.3.3 C3 1/8 v100\n" }, testing::claude).ok);
    const auto moved = firstNoteAt (sync);
    CHECK_MESSAGE (near (moved, 3.25), "first note at " << moved << " s");

    REQUIRE (bus.undo (testing::claude, commands::UndoScope::callersOwn).ok);
    const auto restored = firstNoteAt (sync);
    CHECK_MESSAGE (near (restored, 2.5), "first note at " << restored << " s");
}

TEST_CASE ("a new tempo moves notes in time to keep their beats; a new time signature moves nothing")
{
    // The note is 5 quarter notes in: 2.5 s at 120 BPM.
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", bar, "@length 1bar\n\n1.2 C3 1/8 v100\n") }) }));
    engine::EditSync sync (testEngine(), bus);

    REQUIRE (bus.execute (commands::SetTempo { 90.0, {} }, testing::claude).ok);
    const auto slower = firstNoteAt (sync);
    CHECK_MESSAGE (near (slower, 5 * 60.0 / 90.0), "first note at " << slower << " s");

    // 2/4 halves the bars, so the clip's start is 3.1 and the note's 1.2 of its first bar: the same time.
    REQUIRE (bus.execute (commands::SetTimeSignature { "2/4" }, testing::claude).ok);
    const auto twoFour = firstNoteAt (sync);
    CHECK_MESSAGE (near (twoFour, 5 * 60.0 / 90.0), "first note at " << twoFour << " s");

    REQUIRE (bus.undo (testing::claude, commands::UndoScope::callersOwn).ok);
    REQUIRE (bus.undo (testing::claude, commands::UndoScope::callersOwn).ok);
    const auto restored = firstNoteAt (sync);
    CHECK_MESSAGE (near (restored, 2.5), "first note at " << restored << " s");
}

TEST_CASE ("in any time signature, notes sound where their positions put them, and BPM counts quarter notes")
{
    // At 120 BPM a quarter note is 0.5 s, whatever the signature.
    struct Case
    {
        notes::Meter meter;
        int clipBar;           // the clip starts at <clipBar>.1
        const char* noteLine;  // the clip's one note
        double expectedNote;   // when it should sound, in seconds
        double expectedLoop;   // the loop's length: the clip is one bar long
    };

    const Case cases[] = {
        { { 6, 8 }, 2, "1.4.2 C3 1/8 v100", 1.5 + 0.75 + 0.125, 3.0 },  // bars of 1.5 s; beats are eighths, 0.25 s
        { { 7, 8 }, 2, "1.7 C3 1/16 v100", 1.75 + 1.5, 3.5 },           // bars of 1.75 s
        { { 3, 2 }, 1, "1.2.5 C3 1/8 v100", 1.0 + 0.5, 3.0 },           // beats are half notes, 1 s, with 8 sixteenths
        { { 5, 16 }, 3, "1.5 C3 1/16 v100", 1.25 + 0.5, 1.875 },        // bars of 0.625 s; beats are sixteenths
        { { 3, 4 }, 2, "1.3.4 C3 1/16 v100", 1.5 + 1.0 + 0.375, 3.0 },
    };

    for (const auto& c : cases)
    {
        INFO (c.meter.text() << ", " << c.noteLine);

        const auto start = (c.clipBar - 1) * c.meter.ticksPerBar();
        commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", start, "@length 1bar\n\n" + std::string (c.noteLine) + "\n") }) },
                                              120.0, c.meter));
        engine::EditSync sync (testEngine(), bus);

        CHECK (sync.loopRange().getLength().inSeconds() == doctest::Approx (c.expectedLoop));

        const auto audio = renderLoop (sync);
        const auto sample = firstSoundAt (audio.buffer, 0, 2.5e-4f); // as firstNoteAt: 4OSC is trimmed by 12 dB
        const auto at = sample < 0 ? -1.0 : sample / audio.sampleRate;
        CHECK_MESSAGE (near (at, c.expectedNote), "the note sounded at " << at << " s");
    }
}

TEST_CASE ("a clip written empty renders silent")
{
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 1bar\n\n1.1 C3 1/4 v100\n") }) }));
    engine::EditSync sync (testEngine(), bus);

    REQUIRE (bus.execute (commands::WriteClip { "clp-bass", "@length 1bar\n" }, testing::claude).ok);
    CHECK (firstNoteAt (sync) < 0.0);
}

//==============================================================================
// Structural changes (Milestone 1's step 5). At 120 BPM a bar is 2 s; each
// clip here holds one short note at its start.
namespace
{
    const std::string oneNote = "@length 1bar\n\n1.1 C3 1/8 v100\n";

    // Whether there's sound between these times, in seconds.
    bool soundBetween (const Audio& audio, double from, double to)
    {
        const auto end = std::min (audio.sampleAt (to), audio.buffer.getNumSamples());
        return audio.sampleAt (from) < end && peakBetween (audio.buffer, audio.sampleAt (from), end) > 0.01f;
    }

    // The bars of the loop that start with a note, 1-based.
    std::vector<int> barsWithNotes (engine::EditSync& sync)
    {
        const auto audio = renderLoop (sync);
        std::vector<int> bars;

        for (int number = 1; audio.sampleAt (2.0 * number) <= audio.buffer.getNumSamples(); ++number)
            if (soundBetween (audio, 2.0 * (number - 1), 2.0 * (number - 1) + 0.2))
                bars.push_back (number);

        return bars;
    }

    void run (commands::CommandBus& bus, const commands::Command& command)
    {
        const auto result = bus.execute (command, testing::claude);
        INFO (result.message);
        REQUIRE (result.ok);
    }
}

TEST_CASE ("render: clips added, moved, removed and undone are heard where the model puts them")
{
    commands::CommandBus bus (makeProject ({
        makeTrack ("trk-bass", "Bass", { makeClip ("clp-a", 0, oneNote) }),
        makeTrack ("trk-lead", "Lead", {}),
    }));
    engine::EditSync sync (testEngine(), bus);
    CHECK (barsWithNotes (sync) == std::vector<int> { 1 });

    // A copy on the lead at bar 3, and a clip linked to it at bar 4: the song grows to four bars.
    run (bus, commands::AddClip { "trk-lead", "3.1", commands::CopyOf { "clp-a" }, "clp-copy", {} });
    run (bus, commands::AddClip { "trk-bass", "4.1", commands::LinkedTo { "clp-copy" }, "clp-link", {} });
    CHECK (barsWithNotes (sync) == std::vector<int> { 1, 3, 4 });

    // Moved to bar 2 on the bass, the copy plays there, and the song ends with the linked clip.
    run (bus, commands::MoveClip { "clp-copy", "2.1", "trk-bass" });
    CHECK (barsWithNotes (sync) == std::vector<int> { 1, 2, 4 });

    // Removing the last clip shortens the song again; the linked clip goes silent with its file's notes.
    run (bus, commands::RemoveClip { "clp-link" });
    run (bus, commands::WriteClip { "clp-copy", "@length 1bar\n" });
    CHECK (barsWithNotes (sync) == std::vector<int> { 1 });
    CHECK (renderLoop (sync).buffer.getNumSamples() == renderLoop (sync).sampleAt (4.0)); // two bars, clp-copy's

    while (! bus.history().empty())
        REQUIRE (bus.undo (testing::lee, commands::UndoScope::anyAuthor).ok);

    CHECK (barsWithNotes (sync) == std::vector<int> { 1 });
    CHECK (renderLoop (sync).buffer.getNumSamples() == renderLoop (sync).sampleAt (2.0));
}

TEST_CASE ("render: a track and its instrument added are heard, and removed are silent")
{
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-a", 0, oneNote) }) }));
    engine::EditSync sync (testEngine(), bus);

    // A track with no instrument plays nothing.
    run (bus, commands::AddTrack { "midi", "Keys", {}, {}, {} });
    run (bus, commands::AddClip { "trk-keys", "2.1", commands::ClipText { oneNote }, {}, {} });
    CHECK (barsWithNotes (sync) == std::vector<int> { 1 });

    run (bus, commands::SetInstrument { "trk-keys", commands::Instrument { "hocket:synth", {} } });
    CHECK (barsWithNotes (sync) == std::vector<int> { 1, 2 });

    // Removed, it's silent; back, it plays. (Tracktion won't render an Edit
    // with nothing in it that makes a sound, so one synth always stays.)
    run (bus, commands::RemoveTrack { "trk-keys" });
    CHECK (barsWithNotes (sync) == std::vector<int> { 1 });

    REQUIRE (bus.undo (testing::lee, commands::UndoScope::anyAuthor).ok);
    CHECK (barsWithNotes (sync) == std::vector<int> { 1, 2 });

    run (bus, commands::SetInstrument { "trk-bass", {} });
    CHECK (barsWithNotes (sync) == std::vector<int> { 2 });
}
