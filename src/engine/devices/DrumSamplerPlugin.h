#pragma once

// Hocket's drum sampler as a Tracktion plugin (SPEC §5.2, §8.1, §8.4): what
// EditSync makes for a "hocket:drum-sampler". It turns the MIDI Tracktion
// gives it into DrumEvents for DrumVoices, which plays the hits, and asks a
// KitLibrary for its kit at the rate it plays at.
//
// - Note-ons strike; note-offs and all-notes-off do nothing, so a stop or a
//   seek leaves hits ringing (SPEC §5.2). MIDI's all-sound-off, Tracktion's
//   MIDI panic and a plugin reset fade every hit out.
// - A new kit replaces the one playing once it has loaded, with no graph
//   rebuild; hits from the old kit ring on. Until the first one loads, or
//   with no library, it's silent.
// - Nothing on the audio thread locks or allocates: MIDI is read into a
//   fixed array, sorted by offset in place.

#include "engine/devices/DrumVoices.h"
#include "engine/kits/KitLibrary.h"

#include <tracktion_engine/tracktion_engine.h>

#include <array>
#include <cstddef>
#include <string>

namespace hocket::engine
{
    class DrumSamplerPlugin final : public tracktion::Plugin,
                                    private juce::Timer
    {
    public:
        static inline const char* xmlTypeName = "hocketDrumSampler";

        // The most MIDI events read in one block; any more are left out.
        static constexpr std::size_t maxEventsPerBlock = 1024;

        // Registers the type with the engine's plugin manager; again is harmless.
        static void registerWith (tracktion::Engine&);

        // A new one, with no kit, not yet on a track.
        static juce::ReferenceCountedObjectPtr<DrumSamplerPlugin> create (tracktion::Edit&);

        explicit DrumSamplerPlugin (tracktion::PluginCreationInfo);
        ~DrumSamplerPlugin() override;

        //==============================================================================
        // Message thread.

        // Plays `kit` (as a project names it) from `library`, loaded at the
        // rate this plays at: the kit it had plays on until the new one has
        // loaded. A library of none plays nothing. The library must outlive
        // this, or end first: a library that ends cuts this off.
        void setKit (KitLibrary* library, std::string kit);

        // The kit asked for, as a project names it.
        const std::string& kitName() const { return kit; }

        // The rate the kit is asked for at: the rate this last played at, or
        // the device's before it has played.
        double kitRate() const { return subscribedRate; }

        // The real-time core: for tests, while the audio thread isn't running.
        const DrumVoices& voices() const { return drumVoices; }

        //==============================================================================
        juce::String getName() const override { return "Hocket drum sampler"; }
        juce::String getPluginType() override { return xmlTypeName; }
        juce::String getSelectableDescription() override { return getName(); }
        BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
        int getNumOutputChannelsGivenInputs (int) override { return 2; }
        bool takesMidiInput() override { return true; }
        bool isSynth() override { return true; }
        bool producesAudioWhenNoAudioInput() override { return true; }
        bool canBeDisabled() override { return false; }

        // Tracktion runs it on the message thread only when no graph is
        // playing this plugin. At a new rate, the kit is asked for again at
        // that rate, and nothing plays until it arrives.
        void initialise (const tracktion::PluginInitialisationInfo&) override;
        void deinitialise() override {}

        // Every hit fades out.
        void reset() override;
        void midiPanic() override;

        void applyToBuffer (const tracktion::PluginRenderContext&) override;

    private:
        DrumVoices drumVoices;

        // Message thread's.
        KitLibrary* library = nullptr;
        std::string kit;
        double subscribedRate = 0.0;
        KitLibrary::Subscription subscription;

        // Audio thread's.
        std::array<DrumEvent, maxEventsPerBlock> events {};

        void subscribe (double rate);

        // Frees kits no hit plays from any more.
        void timerCallback() override;
    };
}
