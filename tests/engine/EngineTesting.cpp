#include "EngineTesting.h"
#include "StackSampler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

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
