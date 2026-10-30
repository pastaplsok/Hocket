#pragma once

// Shared setup for the engine tests: one Tracktion Engine for the whole run,
// projects built in code, offline renders, and a fake audio device for testing
// live playback.

#include "core/project/Project.h"
#include "engine/EditSync.h"

#include <tracktion_engine/tracktion_engine.h>

#include <string>
#include <string_view>
#include <vector>

namespace hocket::testing
{
    // Tracktion allows one Engine at a time, so the tests share this one. It
    // never opens a real audio device, and it keeps its settings and temporary
    // files in testFolder(), never in the user's %APPDATA%\Hocket.
    tracktion::Engine& testEngine();

    // A temporary folder with a non-ASCII name, deleted after the run.
    juce::File testFolder();

    //==============================================================================
    inline constexpr notes::Ticks beat = notes::ticksPerBeat;
    inline constexpr notes::Ticks bar = 4 * beat;

    // A clip with the given .notes text, which must be valid.
    project::Clip makeClip (std::string id, notes::Ticks start, std::string_view notesText);

    // A midi track with a 4OSC and the given clips.
    project::Track makeTrack (std::string id, std::string name, std::vector<project::Clip> clips);

    // A 4/4 project.
    project::Project makeProject (std::vector<project::Track> tracks, double tempo = 120.0);

    //==============================================================================
    struct Audio
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;

        int sampleAt (double seconds) const { return static_cast<int> (seconds * sampleRate + 0.5); }
    };

    // Renders the loop range offline, without an audio device.
    Audio renderLoop (engine::EditSync& sync);

    // The first sample at or after `from`, on any channel, louder than the
    // threshold; -1 if there's none.
    int firstSoundAt (const juce::AudioBuffer<float>& buffer, int from = 0, float threshold = 1.0e-3f);

    // The loudest sample in [start, end), on any channel.
    float peakBetween (const juce::AudioBuffer<float>& buffer, int start, int end);

    //==============================================================================
    // Tracktion's hosted audio device: plays through the same live-playback path
    // as a sound card (graph swaps included), but block by block, as fast as the
    // test asks. Open it before creating the EditSync, so its Edit plays here.
    class FakeAudioDevice
    {
    public:
        static constexpr double sampleRate = 44100.0;
        static constexpr int blockSize = 256;

        FakeAudioDevice();
        ~FakeAudioDevice();

        FakeAudioDevice (const FakeAudioDevice&) = delete;
        FakeAudioDevice& operator= (const FakeAudioDevice&) = delete;

        // Plays the next numSamples and adds them to what's been played so far.
        void play (int numSamples);

        // Everything played since the device opened.
        const Audio& output() const { return played; }

    private:
        tracktion::HostedAudioDeviceInterface& device;
        Audio played;
    };
}
