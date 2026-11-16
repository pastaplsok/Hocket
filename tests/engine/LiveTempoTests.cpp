// Live playback through the fake audio device while the tempo and time
// signature change (Milestone 1's step 6, part 3): the playhead keeps its
// place in the bar, the loop holds the song, and a held note carries on.

#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    constexpr int sampleRate = static_cast<int> (FakeAudioDevice::sampleRate);
    constexpr int c3 = 48;

    // A quarter note at this tempo, in samples.
    constexpr int quarter (double bpm)
    {
        return static_cast<int> (sampleRate * 60.0 / bpm);
    }

    // What the app does on the message thread straight after a command.
    void run (commands::CommandBus& bus, engine::EditSync& sync, const commands::Command& command)
    {
        const auto result = bus.execute (command, testing::claude);
        INFO (result.message);
        REQUIRE (result.ok);
        sync.edit().dispatchPendingUpdatesSynchronously();
    }

    // The first note-on, or note-off, for C3 at or after `from`.
    std::optional<std::int64_t> firstC3 (const std::vector<MidiRecorder::Event>& events, bool on, std::int64_t from)
    {
        for (const auto& e : events)
            if (e.sample >= from && e.note == c3 && e.on == on)
                return e.sample;

        return std::nullopt;
    }

    // Any C3 event, or all-notes-off, in [from, to).
    bool anyC3Between (const std::vector<MidiRecorder::Event>& events, std::int64_t from, std::int64_t to)
    {
        return std::ranges::any_of (events, [&] (const MidiRecorder::Event& e) { return e.sample >= from && e.sample < to && (e.note == c3 || e.note < 0); });
    }

    // Within a block.
    bool nearSample (std::optional<std::int64_t> sample, std::int64_t expected)
    {
        return sample.has_value() && std::abs (*sample - expected) <= FakeAudioDevice::blockSize;
    }

    const std::string wholeBar = "@length 1bar\n\n1.1 C3 1/1 v100\n";
    const std::string shortNote = "@length 1bar\n\n1.1 C3 1/8 v100\n";

    // A whole-bar C3 on the bass at 1.1, and a short C3 on the lead at 3.1, so the song is three bars.
    project::Project heldNoteProject()
    {
        return makeProject ({
            makeTrack ("trk-bass", "Bass", { makeClip ("clp-held", 0, wholeBar) }),
            makeTrack ("trk-lead", "Lead", { makeClip ("clp-later", 2 * bar, shortNote) }),
        });
    }

    // Loudness in 20 ms windows from just before `changeAt` to the end,
    // against the steady note before it, within 10%. 4OSC's phases differ
    // from run to run, so loudness can be compared but samples can't.
    void checkSteady (const juce::AudioBuffer<float>& out, int steadyFrom, int changeAt)
    {
        constexpr int window = 882;
        const auto steady = out.getRMSLevel (0, steadyFrom, changeAt - steadyFrom);
        auto quietest = 1.0e9f;
        auto loudest = 0.0f;

        for (int start = changeAt - window; start + window <= out.getNumSamples(); start += window / 2)
        {
            const auto level = out.getRMSLevel (0, start, window) / steady;
            quietest = std::min (quietest, level);
            loudest = std::max (loudest, level);
        }

        CHECK_MESSAGE (quietest > 0.9f, "quietest " << quietest);
        CHECK_MESSAGE (loudest < 1.1f, "loudest " << loudest);
    }
}

TEST_CASE ("live: a slower tempo keeps the playhead's place in the bar, and a held note sounding to its end")
{
    FakeAudioDevice device;
    commands::CommandBus bus (heldNoteProject());
    engine::EditSync sync (testEngine(), bus);
    auto& bass = MidiRecorder::addTo (sync.edit(), 0);
    auto& lead = MidiRecorder::addTo (sync.edit(), 1);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    // At 1.2, to 60 BPM: the held note has three beats left, now of a second
    // each, and the lead's note at 3.1 is seven beats away.
    const auto changeAt = quarter (120);
    device.play (changeAt);
    run (bus, sync, commands::SetTempo { 60.0, {} });
    device.play (7 * quarter (60) + quarter (120));

    // Before stopping, so the note-ends a stop sends aren't in the way.
    const auto bassEvents = bass.recorded();
    const auto leadEvents = lead.recorded();
    INFO ("bass: " << historyOf (c3, bassEvents));
    INFO ("lead: " << historyOf (c3, leadEvents));

    CHECK_FALSE (anyC3Between (bassEvents, 1, changeAt + 3 * quarter (60) - FakeAudioDevice::blockSize));
    CHECK (nearSample (firstC3 (bassEvents, false, 1), changeAt + 3 * quarter (60)));
    CHECK (nearSample (firstC3 (leadEvents, true, 0), changeAt + 7 * quarter (60)));

    // And it sounds as it did, through the change, to just before its end.
    auto out = device.output().buffer;
    out.setSize (2, changeAt + 3 * quarter (60) - FakeAudioDevice::blockSize, true);
    checkSteady (out, changeAt / 2, changeAt);

    REQUIRE (bus.execute (commands::Stop {}, testing::lee).ok);
    device.play (FakeAudioDevice::blockSize);
    CHECK (notesLeftOn (bass.recorded()).empty());
    CHECK (notesLeftOn (lead.recorded()).empty());
}

TEST_CASE ("live: a tempo change on a later pass keeps the playhead's place, and the loop holds the whole song")
{
    struct Case
    {
        double bpm;
        int changeBeat;   // in the song, on the second pass
        int leadBeats;    // from the change to the lead's note at 3.1; 0 if it's passed
        int nextPassBeats; // from the change to the next pass
    };

    // Slower near the end, where the old loop would end before the playhead's
    // new place; faster from 2.3, where the old loop would wrap too late.
    for (const auto c : { Case { 60.0, 10, 0, 2 }, Case { 240.0, 6, 2, 6 } })
    {
        CAPTURE (c.bpm);
        FakeAudioDevice device;
        commands::CommandBus bus (heldNoteProject());
        engine::EditSync sync (testEngine(), bus);
        auto& bass = MidiRecorder::addTo (sync.edit(), 0);
        auto& lead = MidiRecorder::addTo (sync.edit(), 1);
        REQUIRE (timedPlay (bus, testing::lee).ok);

        const auto secondPass = 12 * quarter (120);
        const auto changeAt = secondPass + c.changeBeat * quarter (120);
        device.play (changeAt);
        run (bus, sync, commands::SetTempo { c.bpm, {} });
        CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (12 * 60.0 / c.bpm));
        device.play (c.nextPassBeats * quarter (c.bpm) + quarter (c.bpm) / 2);

        const auto bassEvents = bass.recorded();
        const auto leadEvents = lead.recorded();
        INFO ("bass: " << historyOf (c3, bassEvents));
        INFO ("lead: " << historyOf (c3, leadEvents));

        const auto nextPass = changeAt + c.nextPassBeats * quarter (c.bpm);
        CHECK_FALSE (anyC3Between (bassEvents, changeAt, nextPass - FakeAudioDevice::blockSize));
        CHECK (nearSample (firstC3 (bassEvents, true, changeAt), nextPass));

        if (c.leadBeats > 0)
            CHECK (nearSample (firstC3 (leadEvents, true, changeAt), changeAt + c.leadBeats * quarter (c.bpm)));
        else
            CHECK_FALSE (firstC3 (leadEvents, true, changeAt).has_value());
    }
}

TEST_CASE ("live: a time signature moves nothing in time, and a held note carries on while the loop shortens")
{
    // The lead's clip at 2.2 ends at 3.2, so the song ends at 4.1 in 4/4,
    // after 12 beats; in 2/4 at 6.1, after 10. The change comes on the second
    // pass, during the held note: the playhead has wrapped once, so it counts
    // its place from the start of play, and a new loop length would move it
    // unless it's set again.
    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({
        makeTrack ("trk-bass", "Bass", { makeClip ("clp-held", 0, wholeBar) }),
        makeTrack ("trk-lead", "Lead", { makeClip ("clp-later", 5 * beat, shortNote) }),
    }));
    engine::EditSync sync (testEngine(), bus);
    auto& bass = MidiRecorder::addTo (sync.edit(), 0);
    auto& lead = MidiRecorder::addTo (sync.edit(), 1);
    REQUIRE (timedPlay (bus, testing::lee).ok);

    const auto secondPass = 12 * quarter (120);
    const auto changeAt = secondPass + quarter (120);
    device.play (changeAt);
    run (bus, sync, commands::SetTimeSignature { "2/4" });
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (5.0));
    device.play (10 * quarter (120));

    const auto bassEvents = bass.recorded();
    const auto leadEvents = lead.recorded();
    INFO ("bass: " << historyOf (c3, bassEvents));
    INFO ("lead: " << historyOf (c3, leadEvents));

    // The held note ends at 2.1, as it would have; the lead plays at 3.2 of
    // 2/4, beat 5, as before; then the third pass starts at beat 10.
    CHECK_FALSE (anyC3Between (bassEvents, secondPass + FakeAudioDevice::blockSize, secondPass + 4 * quarter (120) - FakeAudioDevice::blockSize));
    CHECK (nearSample (firstC3 (bassEvents, false, secondPass + 1), secondPass + 4 * quarter (120)));
    CHECK (nearSample (firstC3 (leadEvents, true, changeAt), secondPass + 5 * quarter (120)));
    CHECK (nearSample (firstC3 (bassEvents, true, changeAt), secondPass + 10 * quarter (120)));

    auto out = device.output().buffer;
    out.setSize (2, secondPass + 4 * quarter (120) - FakeAudioDevice::blockSize, true);
    checkSteady (out, secondPass + quarter (120) / 2, changeAt);
}
