// Hocket's drum sampler as a Tracktion plugin, and its kits (Milestone 1's
// step 7, part 4b): MIDI read into hits; offline renders with hits where the
// clip says, at exact levels, and a kit at another rate on time; live, hits
// ringing through a stop and a seek, a kit changed while playing, and a choke
// across the loop's wrap. Kits are made up in a non-ASCII temp folder as
// 32-bit float samples, so every output sample can be worked out exactly.
// DrumVoicesTests.cpp tests the real-time core on its own.

#include "AllocationCounter.h"
#include "EngineTesting.h"
#include "common/TemporaryFolder.h"
#include "common/WavWriter.h"
#include "core/Paths.h"
#include "core/kits/KitCatalog.h"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace hocket;
using namespace hocket::testing;
using engine::DrumSamplerPlugin;

namespace
{
    // At 120 BPM and 44.1 kHz.
    constexpr int quarter = 22050;
    constexpr int barSamples = 4 * quarter;
    constexpr int fadeSamples = 441; // a choke's, 10 ms

    constexpr int kick = 36;
    constexpr int snare = 38;
    constexpr int hatClosed = 42;
    constexpr int hatOpen = 46;
    constexpr int crash = 49;

    // A decaying tone, never exactly zero at its start, so a hit's first
    // sample shows where it starts. Each piece gets its own, so they can't be
    // mistaken for each other.
    std::vector<float> tone (int length, double step, float level)
    {
        std::vector<float> samples (static_cast<std::size_t> (length));

        for (int i = 0; i < length; ++i)
            samples[static_cast<std::size_t> (i)] = level * static_cast<float> (std::sin (step * (i + 1)) * (1.0 - 0.9 * i / length));

        return samples;
    }

    struct TestPiece
    {
        std::string name;
        int note = 0;
        std::vector<std::vector<float>> channels; // one or two
        std::string level = "0dB";
    };

    // A user kit in a folder of its own: kit.json, and each piece's sample as
    // <name>.wav, 32-bit float, so it decodes to exactly these values.
    void writeKit (const std::filesystem::path& folder, const std::vector<TestPiece>& pieces, double rate,
                   const std::vector<std::vector<std::string>>& chokeGroups = {})
    {
        std::filesystem::create_directories (folder);
        std::ostringstream json;
        json << "{\n  \"format\": \"hocket-kit\",\n  \"formatVersion\": 0,\n  \"description\": \"Made up\",\n  \"pieces\": [\n";

        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            const auto& piece = pieces[i];
            json << "    { \"name\": \"" << piece.name << "\", \"note\": " << piece.note << ", \"file\": \"" << piece.name << ".wav\", \"level\": \""
                 << piece.level << "\" }" << (i + 1 < pieces.size() ? ",\n" : "\n");
            writeWav (folder / pathFromUtf8 (piece.name + ".wav"), piece.channels, rate, WavEncoding::float32);
        }

        json << "  ],\n  \"chokeGroups\": [";

        for (std::size_t g = 0; g < chokeGroups.size(); ++g)
        {
            json << (g > 0 ? ", [" : "[");

            for (std::size_t i = 0; i < chokeGroups[g].size(); ++i)
                json << (i > 0 ? ", \"" : "\"") << chokeGroups[g][i] << "\"";

            json << "]";
        }

        json << "]\n}\n";
        std::ofstream (folder / pathFromUtf8 ("kit.json"), std::ios::binary) << json.str();
    }

    // The user's kit folder, a catalog of it and a library over that. Write
    // the kits first; declare it before the EditSync, which it must outlive.
    struct Kits
    {
        TemporaryFolder temp { unicodeName };
        std::filesystem::path userFolder = temp.path / pathFromUtf8 ("Kits");
        std::optional<kits::KitCatalog> catalog;
        std::optional<engine::KitLibrary> library;

        void write (std::string_view name, const std::vector<TestPiece>& pieces, double rate = 44100.0,
                    const std::vector<std::vector<std::string>>& chokeGroups = {})
        {
            writeKit (userFolder / pathFromUtf8 (name), pieces, rate, chokeGroups);
        }

        engine::KitLibrary& open()
        {
            catalog.emplace (std::filesystem::path(), userFolder);
            library.emplace (*catalog);
            return *library;
        }
    };

    // trk-drums with Hocket's drum sampler playing `kit`, and the standard
    // map. Clips are written with note names (C2 for the kick, 36), which
    // parse without the map.
    TestTrack drumsTrack (std::string kit, std::vector<TestClip> clips)
    {
        project::Track track { "trk-drums", "Drums", project::TrackType::drums, { { "dev-drums-kit", project::DeviceType::drumSampler, std::move (kit) } }, {} };
        track.drumMap = notes::standardDrumMap();
        return { std::move (track), std::move (clips) };
    }

    DrumSamplerPlugin& samplerOf (engine::EditSync& sync)
    {
        const auto samplers = tracktion::getAudioTracks (sync.edit())[0]->pluginList.getPluginsOfType<DrumSamplerPlugin>();
        REQUIRE (samplers.size() == 1);
        return *samplers.getFirst();
    }

    // A piece's sample value at `i`, or 0 outside it.
    float at (const std::vector<float>& samples, std::int64_t i)
    {
        return i >= 0 && i < static_cast<std::int64_t> (samples.size()) ? samples[static_cast<std::size_t> (i)] : 0.0f;
    }

    // Compares a channel from `from` to `to` with what it should be, within
    // `tolerance`; empty if it matches, or the first mismatch and how many.
    std::string mismatches (const juce::AudioBuffer<float>& out, int channel, int from, int to, const std::function<float (int)>& expected,
                            float tolerance = 0.0f)
    {
        int count = 0;
        std::ostringstream first;
        first.precision (9);

        for (int i = from; i < to; ++i)
        {
            const auto want = expected (i);
            const auto got = out.getSample (channel, i);

            if (std::abs (got - want) > tolerance && count++ == 0)
                first << "channel " << channel << ", sample " << i << ": " << got << ", should be " << want;
        }

        return count == 0 ? std::string() : first.str() + " (" + std::to_string (count) + " samples off)";
    }

    void run (commands::CommandBus& bus, engine::EditSync& sync, const commands::Command& command)
    {
        const auto result = bus.execute (command, claude);
        INFO (result.message);
        REQUIRE (result.ok);
        sync.edit().dispatchPendingUpdatesSynchronously();
    }
}

//==============================================================================
TEST_CASE ("drum sampler: note-ons strike on their sample, in order of time; note-offs, all-notes-off and velocity 0 do nothing; all-sound-off fades every hit")
{
    // The plugin's MIDI reading, against DrumVoices given the hits it should
    // make: called directly, as Tracktion calls it, with no graph.
    Kits kits;
    const auto kickSample = tone (3000, 0.05, 0.5f);
    const auto snareSample = tone (2000, 0.21, 0.4f);
    kits.write ("test", { { "kick", kick, { kickSample } }, { "snare", snare, { snareSample }, "-6dB" } });
    auto& library = kits.open();

    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", {}) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();
    auto& sampler = samplerOf (sync);
    REQUIRE (sampler.kitRate() == 44100.0);

    constexpr int blockSize = 1024;
    const auto seconds = [] (double samples) { return samples / 44100.0; };
    tracktion::MidiMessageArray midi;
    midi.isAllNotesOff = true; // a stop or a seek: ignored
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, kick, static_cast<juce::uint8> (127)), seconds (100), tracktion::MPESourceID {});
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, snare, static_cast<juce::uint8> (90)), seconds (40.4), tracktion::MPESourceID {}); // earlier, after; rounds to 40
    midi.addMidiMessage (juce::MidiMessage::noteOff (1, kick), seconds (150), tracktion::MPESourceID {});
    midi.addMidiMessage (juce::MidiMessage::allNotesOff (1), seconds (160), tracktion::MPESourceID {});
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, kick, static_cast<juce::uint8> (0)), seconds (170), tracktion::MPESourceID {}); // a note-off
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), seconds (180), tracktion::MPESourceID {}); // no piece there
    midi.addMidiMessage (juce::MidiMessage::allSoundOff (1), seconds (600), tracktion::MPESourceID {});
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, snare, static_cast<juce::uint8> (64)), seconds (blockSize + 7), tracktion::MPESourceID {}); // past the end: its last sample

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    buffer.setSample (0, 5, 0.75f); // what comes in is replaced
    sampler.applyToBuffer (tracktion::PluginRenderContext (&buffer, 0, blockSize, &midi, 0.0, {}, true, false, false, false));

    // The same hits, sorted, straight into DrumVoices.
    engine::DrumVoices reference;
    reference.setKit (std::make_unique<engine::PlayableKit> (
        44100.0, std::vector<engine::PlayableKit::Piece> {
                     { kick, std::make_shared<engine::SampleAudio> (engine::SampleAudio { { kickSample } }), 1.0f, -1 },
                     { snare, std::make_shared<engine::SampleAudio> (engine::SampleAudio { { snareSample } }), static_cast<float> (std::pow (10.0, -6.0 / 20.0)), -1 } }));

    using engine::DrumEvent;
    const std::vector<DrumEvent> events { { DrumEvent::Kind::hit, 40, snare, 90 },
                                          { DrumEvent::Kind::hit, 100, kick, 127 },
                                          { DrumEvent::Kind::silence, 600, 0, 0 },
                                          { DrumEvent::Kind::hit, blockSize - 1, snare, 64 } };
    std::vector<float> left (blockSize, 0.0f);
    std::vector<float> right (blockSize, 0.0f);
    reference.process (left.data(), right.data(), blockSize, events);

    CHECK (mismatches (buffer, 0, 0, blockSize, [&left] (int i) { return left[static_cast<std::size_t> (i)]; }).empty());
    CHECK (mismatches (buffer, 1, 0, blockSize, [&right] (int i) { return right[static_cast<std::size_t> (i)]; }).empty());
    CHECK (buffer.getSample (0, 40) != 0.0f);
    CHECK (buffer.getSample (0, 39) == 0.0f);
    CHECK (sampler.voices().counts().hits == 3);
}

TEST_CASE ("drum sampler: played at another rate, it's silent until the kit has loaded at that rate, then plays it converted")
{
    Kits kits;
    kits.write ("test", { { "kick", kick, { tone (3000, 0.05, 0.5f) } } });
    auto& library = kits.open();

    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", {}) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();
    auto& sampler = samplerOf (sync);
    REQUIRE (sampler.kitRate() == 44100.0);
    const auto conversions = library.counts().conversions;

    // What Tracktion does when a graph starts playing it at 48 kHz.
    sampler.initialise ({ {}, 48000.0, 512 });
    CHECK (sampler.kitRate() == 48000.0);

    constexpr int blockSize = 4096;
    const auto hitOnce = [&sampler]
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        tracktion::MidiMessageArray midi;
        midi.addMidiMessage (juce::MidiMessage::noteOn (1, kick, static_cast<juce::uint8> (127)), 0.0, tracktion::MPESourceID {});
        sampler.applyToBuffer (tracktion::PluginRenderContext (&buffer, 0, blockSize, &midi, 0.0, {}, true, false, false, false));
        return buffer;
    };

    // Not the 44.1 kHz kit, which would play at the wrong pitch.
    const auto before = hitOnce();
    CHECK (before.getMagnitude (0, blockSize) == 0.0f);

    library.waitUntilIdle();
    CHECK (library.counts().conversions == conversions + 1);
    const auto after = hitOnce();
    CHECK (after.getMagnitude (0, blockSize) > 0.1f);

    // 3000 samples at 44.1 kHz are about 3265 at 48.
    int last = 0;

    for (int i = 0; i < blockSize; ++i)
        if (after.getSample (0, i) != 0.0f)
            last = i;

    CHECK (std::abs (last - 3265) <= 2);
}

TEST_CASE ("drum sampler: nothing is allocated or freed reading MIDI and playing hits")
{
    Kits kits;
    kits.write ("test", { { "kick", kick, { tone (3000, 0.05, 0.5f) } }, { "hat-closed", hatClosed, { tone (500, 0.3, 0.3f) } },
                          { "hat-open", hatOpen, { tone (5000, 0.31, 0.3f) } } },
                44100.0, { { "hat-closed", "hat-open" } });
    auto& library = kits.open();

    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", {}) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();
    auto& sampler = samplerOf (sync);

    // Made up front: the test's own allocations aren't the plugin's.
    constexpr int blockSize = 256;
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<tracktion::MidiMessageArray> blocks (60);

    for (std::size_t b = 0; b < blocks.size(); ++b)
    {
        for (int n = 0; n < 12; ++n)
            blocks[b].addMidiMessage (juce::MidiMessage::noteOn (1, n % 3 == 0 ? kick : n % 3 == 1 ? hatClosed : hatOpen, static_cast<juce::uint8> (40 + n * 7)),
                                      (255.0 - n * 20.0) / 44100.0, tracktion::MPESourceID {});

        blocks[b].addMidiMessage (juce::MidiMessage::noteOff (1, kick), 0.001, tracktion::MPESourceID {});

        if (b % 10 == 9)
            blocks[b].addMidiMessage (juce::MidiMessage::allSoundOff (1), 0.002, tracktion::MPESourceID {});
    }

    const CountAllocations count;

    for (auto& midi : blocks)
        sampler.applyToBuffer (tracktion::PluginRenderContext (&buffer, 0, blockSize, &midi, 0.0, {}, true, false, false, false));

    CHECK (count.allocations() == 0);
    CHECK (count.frees() == 0);
    CHECK (sampler.voices().counts().hits == 720);
}

//==============================================================================
TEST_CASE ("render: drum hits land where the clip says, at exactly the level the kit, the velocity and the mix give")
{
    // The render is 24-bit, so a sample is exact to within its last bit.
    Kits kits;
    const auto kickSample = tone (10000, 0.05, 0.6f);
    const auto snareSample = tone (10000, 0.21, 0.5f);
    const auto hatLeft = tone (10000, 0.4, 0.3f);
    const auto hatRight = tone (10000, 0.37, -0.25f);
    kits.write ("test", { { "kick", kick, { kickSample } }, { "snare", snare, { snareSample }, "-6dB" }, { "hat-closed", hatClosed, { hatLeft, hatRight }, "-3.5dB" } });
    auto& library = kits.open();

    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", { makeClip ("clp-beat", 0, "@length 1bar\n\n1.1 C2 1/16 v127\n1.2 D2 1/16 v100\n"
                                                                                                     "1.3 C2 1/16 v28\n1.4 F#2 1/16 v64\n") }) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();

    const auto audio = renderLoop (sync);
    REQUIRE (audio.sampleRate == 44100.0);
    REQUIRE (audio.buffer.getNumSamples() >= barSamples);
    REQUIRE (audio.buffer.getNumChannels() == 2);

    const auto gainOf = [] (const char* level, int velocity) { return static_cast<float> (std::pow (10.0, std::stod (level) / 20.0)) * engine::velocityGain (velocity); };
    const auto kickHard = gainOf ("0", 127);
    const auto snareGain = gainOf ("-6", 100);
    const auto kickGhost = gainOf ("0", 28);
    const auto hatGain = gainOf ("-3.5", 64);

    // Mono samples play the same in both channels; the hat is stereo.
    const auto expected = [&] (int channel, int i)
    {
        return at (kickSample, i) * kickHard + at (snareSample, i - quarter) * snareGain + at (kickSample, i - 2 * quarter) * kickGhost
             + at (channel == 0 ? hatLeft : hatRight, i - 3 * quarter) * hatGain;
    };

    constexpr auto lastBit = 1.5f / 8388608.0f;

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto off = mismatches (audio.buffer, channel, 0, barSamples, [&expected, channel] (int i) { return expected (channel, i); }, lastBit);
        CHECK_MESSAGE (off.empty(), off);
    }
}

TEST_CASE ("render: a kit recorded at 48 kHz, rendered at 44.1 kHz, plays on time")
{
    // An impulse 10 ms into the sample: 480 samples at 48 kHz, 441 at 44.1.
    Kits kits;
    std::vector<float> click (4800, 0.0f);
    click[480] = 0.8f;
    kits.write ("rate", { { "kick", kick, { click } } }, 48000.0);
    auto& library = kits.open();

    commands::CommandBus bus (makeProject ({ drumsTrack ("user/rate", { makeClip ("clp-beat", 0, "@length 1bar\n\n1.1 C2 1/16 v127\n1.3 C2 1/16 v127\n") }) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();
    CHECK (library.counts().conversions == 1);

    const auto audio = renderLoop (sync);
    REQUIRE (audio.sampleRate == 44100.0);

    for (const auto hit : { 0, 2 * quarter })
    {
        CAPTURE (hit);
        int loudest = hit;

        for (int i = hit; i < hit + quarter; ++i)
            if (std::abs (audio.buffer.getSample (0, i)) > std::abs (audio.buffer.getSample (0, loudest)))
                loudest = i;

        CHECK (loudest == hit + 441);
        CHECK (std::abs (audio.buffer.getSample (0, loudest)) > 0.6f);
    }
}

//==============================================================================
TEST_CASE ("live: a drum hit rings through a stop, sample for sample")
{
    Kits kits;
    const auto crashSample = tone (44100, 0.013, 0.5f);
    kits.write ("test", { { "crash", crash, { crashSample } } });
    auto& library = kits.open();

    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", { makeClip ("clp-beat", 0, "@length 2bars\n\n1.1 C#3 1/16 v127\n") }) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();

    REQUIRE (timedPlay (bus, lee).ok);
    device.play (quarter);
    run (bus, sync, commands::Stop {});
    device.play (2 * barSamples);

    // The Edit plays on while stopped, and a stop's all-notes-off leaves a one-shot ringing (SPEC §5.2).
    const auto& out = device.output().buffer;
    const auto start = firstSoundAt (out);
    REQUIRE (start >= 0);
    CHECK (start < FakeAudioDevice::blockSize);

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto off = mismatches (out, channel, start, out.getNumSamples(), [&crashSample, start] (int i) { return at (crashSample, i - start); });
        CHECK_MESSAGE (off.empty(), off);
    }
}

TEST_CASE ("live: a drum hit rings through a seek, sample for sample, and the hit at the new place is struck there")
{
    Kits kits;
    const auto crashSample = tone (44100, 0.013, 0.5f);
    const auto kickSample = tone (5000, 0.05, 0.4f);
    kits.write ("test", { { "crash", crash, { crashSample } }, { "kick", kick, { kickSample } } });
    auto& library = kits.open();

    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", { makeClip ("clp-beat", 0, "@length 4bars\n\n1.1 C#3 1/16 v127\n3.1 C2 1/16 v127\n") }) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();

    REQUIRE (timedPlay (bus, lee).ok);
    device.play (quarter);
    const auto seekAt = device.output().buffer.getNumSamples();
    run (bus, sync, commands::Seek { "3.1" });
    device.play (barSamples);

    // The crash rings on, the kick at 3.1 is struck as the playhead lands
    // there, and nothing else.
    const auto& out = device.output().buffer;
    const auto start = firstSoundAt (out);
    REQUIRE (start >= 0);
    CHECK (start < FakeAudioDevice::blockSize);

    // Where the output first parts from the crash alone. Tracktion's
    // LoopingMidiNode strikes the notes sounding at the new place 0.1 ms after
    // a jump (so they come after controller updates), 4 samples at 44.1 kHz,
    // and a jump that lands a rounding error after the kick's start counts it
    // as sounding.
    int kickAt = -1;

    for (int i = start; i < out.getNumSamples() && kickAt < 0; ++i)
        if (out.getSample (0, i) != at (crashSample, i - start))
            kickAt = i;

    CHECK_MESSAGE ((kickAt >= seekAt && kickAt <= seekAt + 5), "the seek came after sample " << seekAt << "; the kick started at " << kickAt);

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto off = mismatches (out, channel, start, out.getNumSamples(), [&] (int i) { return at (crashSample, i - start) + at (kickSample, i - kickAt); });
        CHECK_MESSAGE (off.empty(), off);
    }
}

TEST_CASE ("live: a new kit while playing replaces the old on the same plugin, from its next hit, while the old kit's hits ring on")
{
    Kits kits;
    const auto crashA = tone (44100, 0.013, 0.5f);
    const auto kickA = tone (5000, 0.05, 0.4f);
    const auto crashB = tone (44100, 0.017, 0.3f);
    const auto kickB = tone (5000, 0.071, 0.35f);
    kits.write ("kit-a", { { "crash", crash, { crashA } }, { "kick", kick, { kickA } } });
    kits.write ("kit-b", { { "crash", crash, { crashB } }, { "kick", kick, { kickB } } });
    auto& library = kits.open();

    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ drumsTrack ("user/kit-a", { makeClip ("clp-beat", 0, "@length 1bar\n\n1.1 C#3 1/16 v127\n1.3 C2 1/16 v127\n") }) }));
    bus.setKitCatalog (&*kits.catalog); // assigning kit B looks it up
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();
    auto* before = &samplerOf (sync);

    REQUIRE (timedPlay (bus, lee).ok);
    device.play (quarter);
    run (bus, sync, commands::SetInstrument { "trk-drums", commands::Instrument { "hocket:drum-sampler", "user/kit-b" } });
    checkEditMatches (sync, bus);
    CHECK (&samplerOf (sync) == before); // no new plugin, so no graph rebuild for it
    CHECK (before->kitName() == "user/kit-b");
    library.waitUntilIdle();
    device.play (quarter + 6000);

    // Kit A's crash from 1.1 rings on; the kick at 1.3 is kit B's.
    const auto& out = device.output().buffer;
    const auto start = firstSoundAt (out);
    REQUIRE (start >= 0);

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto off = mismatches (out, channel, start, out.getNumSamples(), [&] (int i) { return at (crashA, i - start) + at (kickB, i - start - 2 * quarter); });
        CHECK_MESSAGE (off.empty(), off);
    }
}

TEST_CASE ("live: a closed hat at the loop's start chokes the open hat still ringing from the end of the last pass")
{
    // One bar, looped: the open hat at 1.4 rings past the bar's end, and the
    // closed hat at 1.1, in its choke group, cuts it with a 10 ms fade.
    Kits kits;
    const auto closedSample = tone (1000, 0.3, 0.3f);
    const auto openSample = tone (40000, 0.31, 0.4f);
    kits.write ("test", { { "hat-closed", hatClosed, { closedSample } }, { "hat-open", hatOpen, { openSample } } }, 44100.0, { { "hat-closed", "hat-open" } });
    auto& library = kits.open();

    FakeAudioDevice device;
    commands::CommandBus bus (makeProject ({ drumsTrack ("user/test", { makeClip ("clp-beat", 0, "@length 1bar\n\n1.1 F#2 1/16 v127\n1.4 A#2 1/16 v127\n") }) }));
    engine::EditSync sync (testEngine(), bus, &library);
    library.waitUntilIdle();

    REQUIRE (timedPlay (bus, lee).ok);
    device.play (barSamples + quarter);

    const auto& out = device.output().buffer;
    const auto start = firstSoundAt (out);
    REQUIRE (start >= 0);
    const auto openAt = start + 3 * quarter;
    const auto wrap = start + barSamples;

    // The open hat fades from the closed hat's first sample, in a straight
    // line, its first sample at full level, as DrumVoices fades.
    const auto expected = [&] (int i)
    {
        if (i < wrap)
            return at (closedSample, i - start) + at (openSample, i - openAt);

        const auto k = i - wrap;
        const auto open = k < fadeSamples ? at (openSample, i - openAt) * (1.0f * (static_cast<float> (fadeSamples - k) / static_cast<float> (fadeSamples))) : 0.0f;
        return open + at (closedSample, k);
    };

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto off = mismatches (out, channel, start, out.getNumSamples(), expected);
        CHECK_MESSAGE (off.empty(), off);
    }

    // It was still ringing when it was cut.
    CHECK (std::abs (at (openSample, wrap - openAt)) > 0.01f);
}
