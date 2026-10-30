// Offline renders (SPEC §3.3 sync tests): after a command applies, the Edit
// sounds the way the model says.

#include "EngineTesting.h"

#include "core/Paths.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // Where the first note starts in a render, in seconds; -1 if it's silent.
    double firstNoteAt (engine::EditSync& sync)
    {
        const auto audio = renderLoop (sync);
        const auto sample = firstSoundAt (audio.buffer);
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
    CHECK (firstSoundAt (audio.buffer) < audio.sampleAt (0.002));
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

TEST_CASE ("a clip written empty renders silent")
{
    commands::CommandBus bus (makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-bass", 0, "@length 1bar\n\n1.1 C3 1/4 v100\n") }) }));
    engine::EditSync sync (testEngine(), bus);

    REQUIRE (bus.execute (commands::WriteClip { "clp-bass", "@length 1bar\n" }, testing::claude).ok);
    CHECK (firstNoteAt (sync) < 0.0);
}
