// Live playback through the fake audio device while a revert to a kept save
// replaces the whole project (step 9, part 2): the engine changes only what
// changed, so a track the revert leaves as it was plays on untouched.

#include "EngineTesting.h"
#include "common/FakeStorage.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>

using namespace hocket;
using namespace hocket::testing;

namespace
{
    // At 120 BPM and 44.1 kHz.
    constexpr int beatSamples = 22050;

    // C3, MIDI note 48.
    constexpr int heldNote = 48;

    const std::string wholeBar = "@length 1bar\n\n1.1 C3 1/1 v100\n";
    const std::string shortNote = "@length 1bar\n\n1.1 C3 1/8 v100\n";

    // A whole-bar C3 at 1.1.
    TestTrack bass()
    {
        return makeTrack ("trk-bass", "Bass", { makeClip ("clp-held", 0, wholeBar) });
    }

    // A short note at 3.1, unless it says otherwise.
    TestTrack lead (std::string name = "Lead", notes::Ticks start = 2 * bar, std::string_view notesText = shortNote)
    {
        return makeTrack ("trk-lead", std::move (name), { makeClip ("clp-later", start, notesText) });
    }

    // How loud it is in 20 ms windows from `from` to the end, against the
    // steady note before it: quietest and loudest. 4OSC's phases differ from
    // run to run, so loudness can be compared but samples can't.
    std::pair<float, float> loudnessFrom (const juce::AudioBuffer<float>& out, int from)
    {
        constexpr int window = 882;
        const auto steady = out.getRMSLevel (0, beatSamples / 2, beatSamples / 2);
        auto quietest = 1.0e9f;
        auto loudest = 0.0f;

        for (int start = from - window; start + window <= out.getNumSamples(); start += window / 2)
        {
            const auto level = out.getRMSLevel (0, start, window) / steady;
            quietest = std::min (quietest, level);
            loudest = std::max (loudest, level);
        }

        return { quietest, loudest };
    }
}

TEST_CASE ("live: a revert during playback leaves a track it doesn't change playing, with no retrigger, and the Edit matches the model")
{
    struct Save
    {
        const char* what;
        std::function<project::Project()> project;
    };

    const Save saves[] = {
        { "the other track renamed, remixed and its clip moved, and a track added", []
          {
              auto project = makeProject ({ bass(), lead ("Melody", 3 * bar), makeTrack ("trk-keys", "Keys", { makeClip ("clp-keys", 2 * bar, shortNote) }) });
              project.tracks[1].volume = -6.0;
              project.tracks[1].pan = -20;
              return project;
          } },
        { "the other track gone", [] { return makeProject ({ bass() }); } },
        { "the tracks in the other order", [] { return makeProject ({ lead(), bass() }); } },
        { "the other track's notes rewritten", [] { return makeProject ({ bass(), lead ("Lead", 2 * bar, "@length 1bar\n\n1.1 E3 1/8 v90\n") }); } },
        { "the other track a drums track, with the same ID", []
          {
              auto project = makeProject ({ bass(), lead() });
              auto& drums = project.tracks[1];
              drums.type = project::TrackType::drums;
              drums.drumMap = notes::standardDrumMap();
              drums.devices = { { "dev-lead-kit", project::DeviceType::drumSampler, "electronic" } };
              return project;
          } },
    };

    for (const auto& save : saves)
    {
        const std::string what = save.what; // doctest shows a const char* as 1
        CAPTURE (what);
        FakeAudioDevice device;
        FakeStorage storage;
        storage.keep (1, save.project());
        commands::CommandBus bus (makeProject ({ bass(), lead() }));
        bus.setStorage (&storage);
        engine::EditSync sync (testEngine(), bus);
        auto& bassMidi = MidiRecorder::addTo (sync.edit(), 0);
        REQUIRE (timedPlay (bus, lee).ok);

        // The revert at 1.2, then on to just before 1.3. Nothing else sounds in bar 1.
        device.play (beatSamples);
        const auto result = bus.execute (commands::Revert { 1 }, claude);
        INFO (result.message);
        REQUIRE (result.ok);
        sync.edit().dispatchPendingUpdatesSynchronously();

        CHECK (bus.project() == save.project());
        checkEditMatches (sync, bus);
        device.play (beatSamples - FakeAudioDevice::blockSize);

        // The bass's note was struck once, at the start (after the all notes
        // off that play starts with), and nothing has reached it since.
        const auto struckOnce = [] (const std::vector<MidiRecorder::Event>& events)
        {
            INFO (historyOf (heldNote, events));
            REQUIRE_FALSE (events.empty());
            CHECK (events.back().on);
            CHECK (events.back().note == heldNote);
            CHECK (events.back().sample < FakeAudioDevice::blockSize);
            CHECK (std::ranges::count_if (events, [] (const MidiRecorder::Event& e) { return e.on; }) == 1);
        };

        struckOnce (bassMidi.recorded());

        // And it sounds as steadily through the revert as before it.
        const auto [quietest, loudest] = loudnessFrom (device.output().buffer, beatSamples);
        CHECK_MESSAGE (quietest > 0.9f, "quietest " << quietest);
        CHECK_MESSAGE (loudest < 1.1f, "loudest " << loudest);

        // Undone, it's back, and the bass still plays on untouched.
        REQUIRE (bus.undo (lee, commands::UndoScope::anyAuthor).ok);
        sync.edit().dispatchPendingUpdatesSynchronously();
        checkEditMatches (sync, bus);
        device.play (4 * FakeAudioDevice::blockSize);

        struckOnce (bassMidi.recorded());
    }
}

TEST_CASE ("live: a revert that does change a track's sounding note is caught by the same check (the control)")
{
    // The kept save's bass plays E3 instead: the revert ends the C3.
    FakeAudioDevice device;
    FakeStorage storage;
    storage.keep (1, makeProject ({ makeTrack ("trk-bass", "Bass", { makeClip ("clp-held", 0, "@length 1bar\n\n1.1 E3 1/1 v100\n") }), lead() }));
    commands::CommandBus bus (makeProject ({ bass(), lead() }));
    bus.setStorage (&storage);
    engine::EditSync sync (testEngine(), bus);
    auto& bassMidi = MidiRecorder::addTo (sync.edit(), 0);
    REQUIRE (timedPlay (bus, lee).ok);

    device.play (beatSamples);
    REQUIRE (bus.execute (commands::Revert { 1 }, claude).ok);
    sync.edit().dispatchPendingUpdatesSynchronously();
    checkEditMatches (sync, bus);
    device.play (beatSamples - FakeAudioDevice::blockSize);

    const auto events = bassMidi.recorded();
    INFO (historyOf (heldNote, events));
    REQUIRE_FALSE (events.empty());
    CHECK_FALSE (events.back().on);
    CHECK (events.back().sample >= beatSamples);
    CHECK (notesLeftOn (events).empty());
}
