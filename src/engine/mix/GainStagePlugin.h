#pragma once

// A GainStage as a Tracktion plugin: how each track's volume, pan, mute and
// solo are played (SPEC §4.4). EditSync puts one on every track, last of the
// track's own plugins, directly before the volume plugin Tracktion gives
// every track, which it leaves at unity. It isn't a device: it's not in
// project.json, the overview or the API.
//
// Tracktion's volume plugin takes the mix as two parameters, a fader and a
// pan, set one after the other, so an audio block that starts between them
// heads for a gain neither asks for. This takes both channels' gains at once.

#include "engine/mix/GainStage.h"

#include <tracktion_engine/tracktion_engine.h>

namespace hocket::engine
{
    class GainStagePlugin final : public tracktion::Plugin
    {
    public:
        static inline const char* xmlTypeName = "hocketGainStage";

        // Registers the type with the engine's plugin manager; again is harmless.
        static void registerWith (tracktion::Engine&);

        // A new one, at unity, not yet on a track.
        static juce::ReferenceCountedObjectPtr<GainStagePlugin> create (tracktion::Edit&);

        explicit GainStagePlugin (tracktion::PluginCreationInfo);
        ~GainStagePlugin() override;

        // Message thread: both channels' gains, which the audio thread ramps
        // to over 15 ms from its next block.
        void setGains (StereoGain gain) noexcept { stage.setTarget (gain); }
        StereoGain gains() const noexcept { return stage.target(); }

        juce::String getName() const override { return "Hocket gain stage"; }
        juce::String getPluginType() override { return xmlTypeName; }
        juce::String getSelectableDescription() override { return getName(); }
        BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
        int getNumOutputChannelsGivenInputs (int) override { return 2; }
        bool canBeDisabled() override { return false; }
        bool shouldMeasureCpuUsage() const noexcept override { return false; }

        // Tracktion runs it on the message thread only when no graph is
        // playing this plugin: the gains start at their target, with no ramp.
        void initialise (const tracktion::PluginInitialisationInfo&) override;
        void deinitialise() override {}

        void applyToBuffer (const tracktion::PluginRenderContext&) override;

    private:
        GainStage stage;
    };
}
