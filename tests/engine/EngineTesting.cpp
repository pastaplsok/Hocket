#include "EngineTesting.h"
#include "StackSampler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>
#include <tuple>
#include <typeinfo>
#include <vector>

namespace hocket::testing
{
TestClip makeClip (std::string id, notes::Ticks start, std::string_view notesText)
{
    auto file = "clips/" + id.substr (4) + ".notes";
    return { { std::move (id), std::move (file), start }, std::string (notesText) };
}

TestClip linkedClip (std::string id, notes::Ticks start, const TestClip& linkedTo)
{
    return { { std::move (id), linkedTo.clip.file, start }, std::nullopt };
}

TestTrack makeTrack (std::string id, std::string name, std::vector<TestClip> clips)
{
    auto deviceId = "dev-" + id.substr (4) + "-synth";
    return { { std::move (id), std::move (name), project::TrackType::midi, { { std::move (deviceId), project::DeviceType::synth } }, {} },
             std::move (clips) };
}

project::Project makeProject (std::vector<TestTrack> tracks, double tempo, notes::Meter meter)
{
    project::Project project;
    project.tempo = tempo;
    project.meter = meter;

    for (auto& [track, clips] : tracks)
    {
        for (auto& [clip, text] : clips)
        {
            if (text)
            {
                auto parsed = notes::parse (*text, meter);
                INFO (clip.file);
                REQUIRE (parsed.clip);
                project.notesFiles[clip.file] = std::move (*parsed.clip);
            }

            track.clips.push_back (std::move (clip));
        }

        project.tracks.push_back (std::move (track));
    }

    return project;
}

//==============================================================================
ChannelGains specifiedGains (const project::Project& project, const project::Track& track)
{
    const auto anySoloed = std::ranges::any_of (project.tracks, [] (const project::Track& t) { return t.solo; });

    if (track.mute || (anySoloed && ! track.solo))
        return {};

    const auto volume = std::pow (10.0, track.volume / 20.0);
    const auto far = 1.0 - std::abs (track.pan) / 100.0;
    return { track.pan > 0 ? volume * far : volume, track.pan < 0 ? volume * far : volume };
}

ChannelGains volumePluginGains (tracktion::VolumeAndPanPlugin& plugin)
{
    float left = 0.0f;
    float right = 0.0f;
    tracktion::getGainsFromVolumeFaderPositionAndPan (plugin.getSliderPos(), plugin.getPan(), plugin.getPanLaw(), left, right);
    return { left, right };
}

ChannelGains gainStageGains (const engine::GainStagePlugin& stage)
{
    const auto gains = stage.gains();
    return { gains.left, gains.right };
}

bool sameGain (double a, double b)
{
    if (a == 0.0 || b == 0.0)
        return a == b;

    return std::abs (20.0 * std::log10 (a / b)) < 0.001;
}

//==============================================================================
namespace
{
    // Tracktion's beats are quarter notes in every time signature (EditSync.cpp).
    double beatsOf (notes::Ticks ticks)
    {
        return static_cast<double> (ticks) / static_cast<double> (notes::ticksPerQuarterNote);
    }

    double secondsOf (notes::Ticks ticks, const project::Project& project)
    {
        return beatsOf (ticks) * 60.0 / project.tempo;
    }
}

void letEditSettle (FakeAudioDevice& device, engine::EditSync& sync)
{
    device.play (FakeAudioDevice::blockSize);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    device.play (2 * FakeAudioDevice::blockSize);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    CHECK (sync.heldNotes().empty());
    CHECK (sync.endingClips().empty());
}

void checkEditMatches (engine::EditSync& sync, const commands::CommandBus& bus)
{
    auto& edit = sync.edit();
    const auto& project = bus.project();
    const auto& settings = bus.transportSettings();
    const auto played = commands::playRange (project, settings);

    // Played once, the clips end by the end of what plays: a loop region's,
    // or the song's, which no clip passes anyway.
    const auto playingOnce = bus.isPlaying() == true && ! settings.looping;
    const auto clipsEndBy = playingOnce ? played.end : std::numeric_limits<notes::Ticks>::max();
    CAPTURE (playingOnce);
    CAPTURE (played.start);
    CAPTURE (played.end);

    // One tempo, the song's; the time signature stays 4/4, so beats are quarter notes.
    CAPTURE (project.tempo);
    CAPTURE (project.meter.text());
    REQUIRE (edit.tempoSequence.getNumTempos() == 1);
    REQUIRE (edit.tempoSequence.getNumTimeSigs() == 1);
    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == project.tempo);
    CHECK (edit.tempoSequence.getTimeSig (0)->numerator == 4);
    CHECK (edit.tempoSequence.getTimeSig (0)->denominator == 4);

    const auto audioTracks = tracktion::getAudioTracks (edit);
    REQUIRE (static_cast<std::size_t> (audioTracks.size()) == project.tracks.size());

    for (std::size_t i = 0; i < project.tracks.size(); ++i)
    {
        const auto& source = project.tracks[i];
        auto& track = *audioTracks[static_cast<int> (i)];
        CAPTURE (source.id);
        CHECK (track.getName() == juce::String::fromUTF8 (source.name.c_str()));

        // A 4OSC for each hocket:synth, and Hocket's drum sampler for each
        // hocket:drum-sampler, playing the device's kit; first in the chain
        // but for a test's MidiRecorder.
        const auto synths = std::ranges::count_if (source.devices, [] (const project::Device& d) { return d.type == project::DeviceType::synth; });
        const auto samplers = track.pluginList.getPluginsOfType<engine::DrumSamplerPlugin>();
        CHECK (track.pluginList.getPluginsOfType<tracktion::FourOscPlugin>().size() == static_cast<int> (synths));
        int first = 0;

        while (first < track.pluginList.size() && dynamic_cast<MidiRecorder*> (track.pluginList[first]) != nullptr)
            ++first;

        if (synths > 0)
            CHECK (dynamic_cast<tracktion::FourOscPlugin*> (track.pluginList[first]) != nullptr);

        std::vector<std::string> kitsWanted;
        std::vector<std::string> kitsPlayed;

        for (const auto& device : source.devices)
            if (device.type == project::DeviceType::drumSampler)
                kitsWanted.push_back (device.kit);

        for (const auto* sampler : samplers)
            kitsPlayed.push_back (sampler->kitName());

        CHECK (kitsPlayed == kitsWanted);

        if (! samplers.isEmpty())
            CHECK (track.pluginList.indexOf (samplers.getFirst()) == first);

        // The gain stage plays the mix: exactly one, last of the track's own
        // plugins, so after its devices (and any test plugins), and followed
        // only by the volume and meter plugins Tracktion puts on every track.
        const auto stages = track.pluginList.getPluginsOfType<engine::GainStagePlugin>();
        REQUIRE (stages.size() == 1);
        auto* volume = track.getVolumePlugin();
        REQUIRE (volume != nullptr);
        const auto stageAt = track.pluginList.indexOf (stages.getFirst());
        CHECK (track.pluginList.indexOf (volume) == stageAt + 1);
        CHECK (track.pluginList.size() == stageAt + 3);

        if (track.pluginList.size() == stageAt + 3)
            CHECK (dynamic_cast<tracktion::LevelMeterPlugin*> (track.pluginList[stageAt + 2]) != nullptr);

        // Its gains are SPEC §4.4's.
        const auto specified = specifiedGains (project, source);
        const auto mixed = gainStageGains (*stages.getFirst());
        CAPTURE (source.volume);
        CAPTURE (source.pan);
        CAPTURE (source.mute);
        CAPTURE (source.solo);
        CHECK_MESSAGE (sameGain (mixed.left, specified.left), "left " << mixed.left << ", should be " << specified.left);
        CHECK_MESSAGE (sameGain (mixed.right, specified.right), "right " << mixed.right << ", should be " << specified.right);

        // Tracktion's volume plugin is at unity, exactly, and isn't automated:
        // 0 dB, the centre, and the linear pan law, whose centre is unity.
        CHECK (volume->getPanLaw() == tracktion::PanLawLinear);
        CHECK (volume->getPan() == 0.0f);
        CHECK (volume->getSliderPos() == tracktion::decibelsToVolumeFaderPosition (0.0f));
        const auto unity = volumePluginGains (*volume);
        CHECK (unity.left == 1.0);
        CHECK (unity.right == 1.0);
        CHECK_FALSE (volume->volParam->hasAutomationPoints());
        CHECK_FALSE (volume->panParam->hasAutomationPoints());

        // Every clip that starts before the end, and no other.
        std::vector<const project::Clip*> expectedClips;

        for (const auto& clip : source.clips)
            if (clip.start < clipsEndBy)
                expectedClips.push_back (&clip);

        // And clips taken out during playback, emptied, until a block has
        // played them so (EditSync::endingClips): each is one of EditSync's,
        // named for its clip with " (ending)", and has no notes.
        auto trackClips = track.getClips();
        const auto& ending = sync.endingClips();

        for (int c = trackClips.size(); --c >= 0;)
        {
            auto* clip = dynamic_cast<tracktion::MidiClip*> (trackClips[c]);

            if (clip == nullptr || std::ranges::find (ending, clip, &tracktion::MidiClip::Ptr::get) == ending.end())
                continue;

            CAPTURE (clip->getName().toStdString());
            CHECK (clip->getName().endsWith (" (ending)"));
            CHECK (clip->getSequence().getNumNotes() == 0);
            trackClips.remove (c);
        }

        REQUIRE (static_cast<std::size_t> (trackClips.size()) == expectedClips.size());

        for (const auto* clip : expectedClips)
        {
            CAPTURE (clip->id);
            const auto found = std::ranges::find_if (trackClips, [clip] (tracktion::Clip* c) { return c->getName() == juce::String (clip->id); });
            REQUIRE (found != trackClips.end());

            auto* midi = dynamic_cast<tracktion::MidiClip*> (*found);
            REQUIRE (midi != nullptr);
            const auto end = std::min (project::clipEnd (project, *clip), clipsEndBy);
            CHECK (midi->getStartBeat().inBeats() == doctest::Approx (beatsOf (clip->start)));
            CHECK (midi->getEndBeat().inBeats() == doctest::Approx (beatsOf (end)));
            CHECK (midi->getPosition().getStart().inSeconds() == doctest::Approx (secondsOf (clip->start, project)));
            CHECK (midi->getPosition().getEnd().inSeconds() == doctest::Approx (secondsOf (end, project)));

            // Every note, even those past the end, which the clip's end cuts
            // off; but for those EditSync holds back until a block has played
            // without them (SPEC §3.3), each of which must sound where the
            // playhead was, or would be, when they were: where a wrap landed,
            // or just ahead when a clip was made.
            using Note = std::tuple<int, double, double, int>; // pitch, start, length, velocity
            std::vector<Note> expected;
            std::vector<Note> actual;
            const auto heldHere = sync.heldNotes().find (clip->id);
            const auto& stretches = sync.heldWhere();

            for (const auto& note : project::notesOf (project, *clip).notes)
            {
                const auto isHeld = heldHere != sync.heldNotes().end()
                                 && std::ranges::find (heldHere->second, engine::EditSync::HeldNote { note.start, note.pitch }) != heldHere->second.end();

                if (! isHeld)
                {
                    expected.emplace_back (note.pitch, beatsOf (note.start), beatsOf (note.length), note.velocity);
                    continue;
                }

                const auto start = clip->start + note.start;
                const auto noteEnd = std::min (start + note.length, end);
                CAPTURE (note.pitch);
                CAPTURE (start);
                CHECK_MESSAGE (std::ranges::any_of (stretches, [start, noteEnd] (commands::TickRange at) { return start < at.end && noteEnd > at.start; }),
                               "a note held back that doesn't sound where the playhead was when it was");
            }

            if (heldHere != sync.heldNotes().end())
                CHECK (heldHere->second.size() + expected.size() == project::notesOf (project, *clip).notes.size());

            for (const auto* note : midi->getSequence().getNotes())
                actual.emplace_back (note->getNoteNumber(), note->getStartBeat().inBeats(), note->getLengthBeats().inBeats(), note->getVelocity());

            std::ranges::sort (expected);
            std::ranges::sort (actual);
            CHECK (actual == expected);
        }
    }

    // Only clips the Edit has hold notes back.
    for (const auto& [clipId, notes] : sync.heldNotes())
    {
        CAPTURE (clipId);
        CHECK (project::findClip (project, clipId) != nullptr);
        CHECK_FALSE (notes.empty());
    }

    CHECK (sync.loopRange().getStart().inSeconds() == doctest::Approx (secondsOf (played.start, project)));
    CHECK (sync.loopRange().getEnd().inSeconds() == doctest::Approx (secondsOf (played.end, project)));

    // Nothing of EditSync's in Tracktion's undo, which it clears after every
    // change it makes. But Tracktion records moves of its own there later: on
    // the message loop, after clips are added, moved or resized, it sorts each
    // track's clips by start (ClipOwner's async update). So a test that ran
    // the message loop may find child moves there, and nothing else.
    juce::Array<const juce::UndoableAction*> actions;
    edit.getUndoManager().getActionsInCurrentTransaction (actions);
    const auto notSorting = std::ranges::count_if (actions, [] (const juce::UndoableAction* action)
    {
        return std::string_view (typeid (*action).name()).find ("MoveChildAction") == std::string_view::npos;
    });
    CHECK_MESSAGE (notSorting == 0, "Tracktion's undo holds " << notSorting << " change(s) other than its own sort of clips");
}

//==============================================================================
Audio renderLoop (engine::EditSync& sync)
{
    auto& edit = sync.edit();
    const auto file = testFolder().getNonexistentChildFile ("render", ".wav");
    const auto tracks = tracktion::toBitSet (tracktion::getAllTracks (edit));

    REQUIRE (tracktion::Renderer::renderToFile ({}, file, edit, sync.loopRange(), tracks, true, false, {}, false));

    Audio audio;
    {
        std::unique_ptr<juce::AudioFormatReader> reader (edit.engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));
        REQUIRE (reader != nullptr);

        audio.sampleRate = reader->sampleRate;
        audio.buffer.setSize (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
        REQUIRE (reader->read (&audio.buffer, 0, audio.buffer.getNumSamples(), 0, true, true));
    }

    file.deleteFile();
    return audio;
}

int firstSoundAt (const juce::AudioBuffer<float>& buffer, int from, float threshold)
{
    for (int i = std::max (0, from); i < buffer.getNumSamples(); ++i)
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            if (std::abs (buffer.getSample (channel, i)) > threshold)
                return i;

    return -1;
}

float peakBetween (const juce::AudioBuffer<float>& buffer, int start, int end)
{
    start = std::clamp (start, 0, buffer.getNumSamples());
    end = std::clamp (end, start, buffer.getNumSamples());
    return start == end ? 0.0f : buffer.getMagnitude (start, end - start);
}

//==============================================================================
namespace
{
    // Opening and closing a device posts Tracktion updates, and handling one
    // stops any transport that's playing. Tests that don't run the message
    // loop would leave them for the next test that does, so handle them now.
    void handlePendingMessages()
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
}

FakeAudioDevice::FakeAudioDevice() : device (testEngine().getDeviceManager().getHostedAudioDeviceInterface())
{
    handlePendingMessages();
    auto& deviceManager = testEngine().getDeviceManager();

    // Blocks come as fast as the test makes them, so measured CPU use is
    // meaningless; without this Tracktion would mute blocks it thinks overran.
    deviceManager.setCpuLimitBeforeMuting (1000.0);

    tracktion::HostedAudioDeviceInterface::Parameters parameters;
    parameters.sampleRate = sampleRate;
    parameters.blockSize = blockSize;
    device.initialise (parameters);
    device.prepareToPlay (sampleRate, blockSize);
    deviceManager.dispatchPendingUpdates();
    handlePendingMessages();

    played.sampleRate = sampleRate;
    played.buffer.setSize (2, 0);
}

FakeAudioDevice::~FakeAudioDevice()
{
    auto& deviceManager = testEngine().getDeviceManager();
    deviceManager.deviceManager.closeAudioDevice();
    deviceManager.removeHostedAudioDeviceInterface();
    handlePendingMessages();
}

void FakeAudioDevice::play (int numSamples)
{
    const auto start = played.buffer.getNumSamples();
    played.buffer.setSize (2, start + numSamples, true, true);
    juce::MidiBuffer midi;

    // Inputs are read from the block and outputs written over them; the new samples start silent.
    for (int offset = 0; offset < numSamples; offset += blockSize)
    {
        juce::AudioBuffer<float> block (played.buffer.getArrayOfWritePointers(), 2, start + offset, std::min (blockSize, numSamples - offset));
        device.processBlock (block, midi);
    }
}

double FakeAudioDevice::playBlock (juce::AudioBuffer<float>& destination, int start)
{
    jassert (destination.getNumChannels() == 2 && start + blockSize <= destination.getNumSamples());

    // A view of the destination, so nothing is allocated; it starts silent, as in play().
    juce::AudioBuffer<float> block (destination.getArrayOfWritePointers(), 2, start, blockSize);
    block.clear();
    midiScratch.clear();

    const auto before = std::chrono::steady_clock::now();
    device.processBlock (block, midiScratch);
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - before).count();
}

namespace
{
    bool switchedOn (const char* name)
    {
        return juce::SystemStats::getEnvironmentVariable (name, {}) == "1";
    }
}

commands::Result timedPlay (commands::CommandBus& bus, const commands::Author& author)
{
    if (switchedOn ("HOCKET_TEST_MIDI_OUTPUTS_OFF"))
    {
        auto& devices = testEngine().getDeviceManager();

        for (int i = 0; i < devices.getNumMidiOutDevices(); ++i)
            if (auto* output = devices.getMidiOutDevice (i); output != nullptr && output->isEnabled())
                output->setEnabled (false);
    }

    std::unique_ptr<StackSampler> sampler;

    if (switchedOn ("HOCKET_TEST_SAMPLE_PLAY"))
        sampler = std::make_unique<StackSampler>();

    const auto start = std::chrono::steady_clock::now();
    auto result = bus.execute (commands::Play {}, author);
    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();

    if (switchedOn ("HOCKET_TEST_TIME_PLAY"))
    {
        // Tracktion's play() polls with Thread::sleep (1) until an audio block
        // has been played, up to 200 times, so its time follows the timer's.
        const auto sleepStart = std::chrono::steady_clock::now();

        for (int i = 0; i < 10; ++i)
            juce::Thread::sleep (1);

        const auto sleepMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - sleepStart).count() / 10.0;
        std::cout << "play took " << seconds << " s; a 1 ms sleep takes " << sleepMs << " ms\n";
    }

    if (seconds > 1.5)
        std::cout << "\nWARNING: play() took " << seconds << " s, where it usually takes about 0.55 s. Before anything else changes, "
                     "note what's using the audio devices (which are connected, the Magni among them, and what else is playing), "
                     "for PROGRESS.md's loose end about the slow play().\n\n";

    if (sampler != nullptr)
        std::cout << sampler->stop() << std::endl;

    return result;
}
}
