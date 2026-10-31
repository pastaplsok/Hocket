#include "EngineTesting.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace hocket::testing
{
project::Clip makeClip (std::string id, notes::Ticks start, std::string_view notesText)
{
    auto parsed = notes::parse (notesText);
    REQUIRE (parsed.clip);
    return { id, "clips/" + id.substr (4) + ".notes", start, std::move (*parsed.clip) };
}

project::Track makeTrack (std::string id, std::string name, std::vector<project::Clip> clips)
{
    auto deviceId = "dev-" + id.substr (4) + "-synth";
    return { std::move (id), std::move (name), project::TrackType::midi, { { std::move (deviceId), project::DeviceType::fourOsc } }, std::move (clips) };
}

project::Project makeProject (std::vector<project::Track> tracks, double tempo)
{
    project::Project project;
    project.tempo = tempo;
    project.tracks = std::move (tracks);
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
}
