// Bug sweep, area 2 (the engine): each finding as a test, written before its
// fix (PROGRESS.md, "Bug sweep").

#include "DrumTesting.h"
#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <thread>

using namespace hocket;
using namespace hocket::testing;
using namespace std::chrono_literals;

namespace
{
    // At 120 BPM and 44.1 kHz.
    constexpr int quarter = 22050;
    constexpr int barSamples = 4 * quarter;
    constexpr int blockSize = FakeAudioDevice::blockSize;
    constexpr int kick = 36;
    constexpr int e3 = 52;

    const std::string wholeBarE3 = "@length 1bar\n\n1.1 E3 1/1 v100\n";

    // The samples where `note` is struck, in [from, to).
    std::vector<std::int64_t> struck (const std::vector<MidiRecorder::Event>& events, int note, std::int64_t from, std::int64_t to)
    {
        std::vector<std::int64_t> found;

        for (const auto& e : events)
            if (e.note == note && e.on && e.sample >= from && e.sample < to)
                found.push_back (e.sample);

        return found;
    }

    // Three bars, by trk-end's clip at 3.1, which has no instrument: trk-lead
    // with a synth, trk-drums with a drum sampler whose kit isn't installed
    // (silent, but its MIDI is recorded), and with `extra`, trk-extra, a
    // synth holding E3 through bar 1. Nothing sounds in bars 1 and 2.
    project::Project threeBars (std::vector<TestClip> leadClips, bool extra)
    {
        auto end = makeTrack ("trk-end", "End", { makeClip ("clp-end", 2 * bar, "@length 1bar\n\n1.1 C3 1/16 v100\n") });
        end.track.devices.clear();
        std::vector<TestTrack> tracks { makeTrack ("trk-lead", "Lead", std::move (leadClips)), drumsTrack ("not-installed", {}), std::move (end) };

        if (extra)
            tracks.push_back (makeTrack ("trk-extra", "Extra", { makeClip ("clp-extra", 0, wholeBarE3) }));

        return makeProject (std::move (tracks));
    }
}

//==============================================================================
// Finding 1: a clip that appears under the playhead struck the notes it had
// sounding there at once, part-way through (Tracktion's LoopingMidiNode, new,
// has no node before it to take sounding notes from). SPEC §3.3: a note a
// change adds starts the next time the playhead reaches its start, on every
// instrument. Each change comes at 1.2, inside a note that starts at 1.1.
TEST_CASE ("sweep: a clip that appears under the playhead, added, moved there or brought back by undo, waits for its notes' starts")
{
    using Change = std::function<commands::Result (commands::CommandBus&)>;

    struct Case
    {
        const char* what;
        std::vector<TestClip> leadClips;
        bool extra = false;
        std::vector<commands::Command> setUp; // before play
        Change change;
        int note = e3;
        int recorderTrack = 0; // -1: listen to the output instead
    };

    const Case cases[] = {
        { "a clip added", {}, false, {},
          [] (commands::CommandBus& bus) { return bus.execute (commands::AddClip { "trk-lead", "1.1", commands::ClipText { wholeBarE3 }, {}, {} }, claude); } },
        { "a clip moved there", { makeClip ("clp-a", bar, wholeBarE3) }, false, {},
          [] (commands::CommandBus& bus) { return bus.execute (commands::MoveClip { "clp-a", "1.1", {} }, claude); } },
        { "a clip moved there from another track", {}, false, { commands::AddClip { "trk-end", "2.1", commands::ClipText { wholeBarE3 }, "clp-a", {} } },
          [] (commands::CommandBus& bus) { return bus.execute (commands::MoveClip { "clp-a", "1.1", "trk-lead" }, claude); } },
        { "a removed clip brought back by undo", { makeClip ("clp-a", 0, wholeBarE3) }, false, { commands::RemoveClip { "clp-a" } },
          [] (commands::CommandBus& bus) { return bus.undo (claude, commands::UndoScope::anyAuthor); } },
        { "a drum clip added, on the drum sampler", {}, false, {},
          [] (commands::CommandBus& bus) { return bus.execute (commands::AddClip { "trk-drums", "1.1", commands::ClipText { "@length 1bar\n\n1.1 C2 1/2 v100\n" }, {}, {} }, claude); },
          kick, 1 },
        { "a removed track brought back by undo, with its clip", {}, true, { commands::RemoveTrack { "trk-extra" } },
          [] (commands::CommandBus& bus) { return bus.undo (claude, commands::UndoScope::anyAuthor); }, e3, -1 },
    };

    for (const auto& c : cases)
    {
        const std::string what = c.what;
        CAPTURE (what);
        FakeAudioDevice device;
        commands::CommandBus bus (threeBars (c.leadClips, c.extra));
        engine::EditSync sync (testEngine(), bus);

        for (const auto& command : c.setUp)
            REQUIRE (bus.execute (command, claude).ok);

        auto* recorder = c.recorderTrack >= 0 ? &MidiRecorder::addTo (sync.edit(), c.recorderTrack) : nullptr;
        sync.edit().dispatchPendingUpdatesSynchronously();
        REQUIRE (timedPlay (bus, lee).ok);

        device.play (quarter); // to 1.2
        const auto result = c.change (bus);
        INFO (result.message);
        REQUIRE (result.ok);
        sync.edit().dispatchPendingUpdatesSynchronously(); // as the app's message thread would, straight after
        letEditSettle (device, sync);
        device.play (3 * barSamples - 3 * blockSize); // round to 1.2 again

        const auto nextPass = 3 * barSamples;

        if (recorder != nullptr)
        {
            const auto events = recorder->recorded();
            INFO (historyOf (c.note, events));
            CHECK (struck (events, c.note, quarter, nextPass - blockSize).empty());
            const auto atStart = struck (events, c.note, nextPass - blockSize, nextPass + quarter);
            CHECK (atStart.size() == 1);
            CHECK ((! atStart.empty() && std::abs (atStart.front() - nextPass) <= blockSize));
        }
        else
        {
            const auto& out = device.output().buffer;
            CHECK (peakBetween (out, quarter + 2 * blockSize, nextPass - blockSize) < 1.0e-4f);
            CHECK (peakBetween (out, nextPass, nextPass + quarter) > 1.0e-3f);
        }
    }
}

//==============================================================================
// Finding 2: a kit that wasn't there when a track's sampler asked for it
// stayed silent until a restart, even once installed and assigned again.
TEST_CASE ("sweep: a kit installed after its track asked for it is loaded once it's there")
{
    Kits kits;
    kits.open(); // no kits yet
    commands::CommandBus bus (makeProject ({ drumsTrack ("user/late", { makeClip ("clp-beat", 0, "@length 1bar\n\n1.1 C2 1/16 v127\n") }) }));
    bus.setKitCatalog (&*kits.catalog);

    FakeAudioDevice device;
    engine::EditSync sync (testEngine(), bus, &*kits.library);
    kits.library->waitUntilIdle();

    const auto kickSample = tone (2000, 0.05, 0.5f);
    kits.write ("late", { { "kick", kick, { kickSample } } });

    // The library's looks, every half second.
    kits.library->checkForChanges();
    kits.library->checkForChanges();
    kits.library->waitUntilIdle();

    REQUIRE (timedPlay (bus, lee).ok);
    device.play (barSamples / 2);
    CHECK (mismatches (device.output().buffer, 0, 0, 4000, [&kickSample] (int i) { return at (kickSample, i); }, 1.0e-6f) == "");
    REQUIRE (bus.execute (commands::Stop {}, lee).ok);
}

TEST_CASE ("sweep: a user kit that's broken when a track asks for it is loaded once it's fixed")
{
    Kits kits;
    kits.write ("fixed", { { "kick", kick, { tone (2000, 0.05, 0.5f) } } });
    std::ofstream (kits.userFolder / pathFromUtf8 ("fixed") / pathFromUtf8 ("kit.json"), std::ios::binary) << "{ not a manifest";
    kits.open();

    std::vector<std::unique_ptr<engine::PlayableKit>> delivered;
    const auto subscription = kits.library->subscribe ("user/fixed", 44100.0, [&delivered] (std::unique_ptr<engine::PlayableKit> kit) { delivered.push_back (std::move (kit)); });
    kits.library->waitUntilIdle();
    REQUIRE (delivered.size() == 1);
    CHECK (delivered.back()->pieceOn (kick) == nullptr); // silent

    kits.write ("fixed", { { "kick", kick, { tone (2000, 0.05, 0.5f) } } });
    kits.library->checkForChanges();
    kits.library->checkForChanges();
    kits.library->waitUntilIdle();

    REQUIRE (delivered.size() == 2);
    CHECK (delivered.back()->pieceOn (kick) != nullptr);
}

//==============================================================================
// Finding 4: a user kit's kit.json saved after the catalog read it, but
// before a track first used it, played as the catalog read it, for good.
TEST_CASE ("sweep: a user kit's kit.json saved before its first use is taken in")
{
    Kits kits;
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) }, "0dB" } });
    kits.open(); // the catalog reads it: 0dB

    std::this_thread::sleep_for (20ms);
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) }, "-12dB" } });

    std::vector<float> gains;
    const auto subscription = kits.library->subscribe ("user/k", 44100.0, [&gains] (std::unique_ptr<engine::PlayableKit> kit)
    {
        if (kit != nullptr && kit->pieceOn (kick) != nullptr)
            gains.push_back (kit->pieceOn (kick)->gain);
    });

    kits.library->waitUntilIdle();
    kits.library->checkForChanges();
    kits.library->checkForChanges();
    kits.library->waitUntilIdle();

    REQUIRE_FALSE (gains.empty());
    CHECK (gains.back() == doctest::Approx (0.251189f)); // -12 dB
}

TEST_CASE ("sweep: a track taking a watched kit doesn't hide a change from the tracks already playing it")
{
    Kits kits;
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) }, "0dB" } });
    kits.open();

    std::vector<float> first;
    const auto a = kits.library->subscribe ("user/k", 44100.0, [&first] (std::unique_ptr<engine::PlayableKit> kit) { first.push_back (kit->pieceOn (kick)->gain); });
    kits.library->waitUntilIdle();
    REQUIRE (first.size() == 1);

    // Saved, and a second track takes the kit before the library has looked twice.
    std::this_thread::sleep_for (20ms);
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) }, "-12dB" } });
    kits.library->checkForChanges();
    const auto b = kits.library->subscribe ("user/k", 44100.0, [] (std::unique_ptr<engine::PlayableKit>) {});
    kits.library->waitUntilIdle();

    kits.library->checkForChanges();
    kits.library->checkForChanges();
    kits.library->waitUntilIdle();

    CHECK (first.back() == doctest::Approx (0.251189f));
}

//==============================================================================
// Finding 3: libsamplerate refuses a ratio over 256, and nothing on the
// loader thread caught its exception, so the process ended. A sample at a
// rate it can't take is refused when the kit is read; anything that goes
// wrong while a kit loads leaves it silent, saying why.
TEST_CASE ("sweep: a kit with a sample at a rate the converter can't take can't be used, and says why")
{
    Kits kits;
    kits.write ("lowrate", { { "kick", kick, { tone (50, 0.5, 0.5f) } } }, 100.0);
    kits.open();

    const auto found = kits.catalog->find ("user/lowrate");
    CHECK (found.kit == nullptr);
    CHECK (found.problem.find ("100 Hz") != std::string::npos);
}

TEST_CASE ("sweep: a kit that can't be converted to the rate it's asked for is silent, with why, and the process carries on")
{
    Kits kits;
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) } } });
    kits.open();

    // 44.1 kHz samples at 30 Hz: a ratio of 1/1470, past libsamplerate's 1/256.
    std::vector<std::unique_ptr<engine::PlayableKit>> delivered;
    const auto subscription = kits.library->subscribe ("user/k", 30.0, [&delivered] (std::unique_ptr<engine::PlayableKit> kit) { delivered.push_back (std::move (kit)); });
    kits.library->waitUntilIdle();

    REQUIRE (delivered.size() == 1);
    CHECK (delivered.front()->pieceOn (kick) == nullptr);

    const auto& report = kits.catalog->find ("user/k").kit->loaded;
    REQUIRE (report.problems.size() == 1);
    CHECK (report.problems.front().find ("couldn't be loaded") != std::string::npos);
    CHECK (report.failed.find ("libsamplerate can't convert from 44100") != std::string::npos);

    // The library carries on: at a rate it can convert to, the kit plays.
    const auto again = kits.library->subscribe ("user/k", 48000.0, [&delivered] (std::unique_ptr<engine::PlayableKit> kit) { delivered.push_back (std::move (kit)); });
    kits.library->waitUntilIdle();
    REQUIRE (delivered.size() == 2);
    CHECK (delivered.back()->pieceOn (kick) != nullptr);
}

//==============================================================================
// Smaller, by reading: the cache kept every version of every sample it had
// loaded, for the session. It keeps only what a kit still uses.
TEST_CASE ("sweep: the kit library's cache keeps only samples a kit still uses")
{
    Kits kits;
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) } } });
    kits.open();

    // As a sampler does: it keeps the kit it plays, and lets the one before go.
    std::unique_ptr<engine::PlayableKit> playing;
    const auto subscription = kits.library->subscribe ("user/k", 44100.0, [&playing] (std::unique_ptr<engine::PlayableKit> kit) { playing = std::move (kit); });
    kits.library->waitUntilIdle();
    CHECK (kits.library->counts().cached == 1);

    const auto lookTwice = [&kits]
    {
        kits.library->checkForChanges();
        kits.library->checkForChanges();
        kits.library->waitUntilIdle();
    };

    // The sample saved again, three times: each new version is decoded, and
    // the one before goes once the kit that used it has.
    for (int save = 1; save <= 3; ++save)
    {
        CAPTURE (save);
        std::this_thread::sleep_for (20ms);
        kits.write ("k", { { "kick", kick, { tone (2000 + save * 10, 0.05, 0.5f) } } });
        lookTwice();
        REQUIRE (playing != nullptr);
        CHECK (playing->pieceOn (kick)->length == static_cast<std::size_t> (2000 + save * 10));
        CHECK (kits.library->counts().cached <= 2); // this version, and perhaps the last until its kit had gone
    }

    // A level, in kit.json alone, which decodes nothing: by now only the
    // latest version is kept.
    std::this_thread::sleep_for (20ms);
    const auto manifest = kits.userFolder / pathFromUtf8 ("k") / pathFromUtf8 ("kit.json");
    std::string text;
    {
        std::ifstream in (manifest, std::ios::binary);
        text.assign (std::istreambuf_iterator<char> (in), {});
    }
    text.replace (text.find ("\"0dB\""), 5, "\"-3dB\"");
    std::ofstream (manifest, std::ios::binary) << text;
    const auto decodedBefore = kits.library->counts().decodes;
    lookTwice();
    CHECK (kits.library->counts().decodes == decodedBefore);
    CHECK (playing->pieceOn (kick)->gain == doctest::Approx (0.707946f));
    CHECK (kits.library->counts().cached == 1);
}

// Smaller, by reading: a drum sampler kept a raw pointer to its library, and
// would call into it after it had ended. It holds a weak reference.
TEST_CASE ("sweep: a drum sampler whose kit library has ended plays nothing, and carries on")
{
    Kits kits;
    kits.write ("k", { { "kick", kick, { tone (2000, 0.05, 0.5f) } } });
    kits.open();
    commands::CommandBus bus (makeProject ({ drumsTrack ("user/k", {}) }));
    bus.setKitCatalog (&*kits.catalog);
    engine::EditSync sync (testEngine(), bus, &*kits.library);
    kits.library->waitUntilIdle();

    auto& sampler = samplerOf (sync);
    kits.library.reset();

    // At a new rate it asks its library again: there's none now.
    sampler.initialise ({ tracktion::TimePosition(), 48000.0, 256 });
    CHECK (sampler.kitRate() == 48000.0);
    sampler.setKit (nullptr, "user/k");
    CHECK (sampler.kitName() == "user/k");
}
