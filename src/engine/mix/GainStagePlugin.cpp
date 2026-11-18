#include "engine/mix/GainStagePlugin.h"

#include <stdexcept>

namespace hocket::engine
{
void GainStagePlugin::registerWith (tracktion::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<GainStagePlugin>();
}

juce::ReferenceCountedObjectPtr<GainStagePlugin> GainStagePlugin::create (tracktion::Edit& edit)
{
    juce::ReferenceCountedObjectPtr<GainStagePlugin> plugin (dynamic_cast<GainStagePlugin*> (edit.getPluginCache().createNewPlugin (xmlTypeName, {}).get()));

    if (plugin == nullptr)
        throw std::runtime_error ("Tracktion couldn't create Hocket's gain stage: is its type registered with the engine?");

    return plugin;
}

GainStagePlugin::GainStagePlugin (tracktion::PluginCreationInfo info) : Plugin (info) {}

GainStagePlugin::~GainStagePlugin()
{
    notifyListenersOfDeletion();
}

void GainStagePlugin::initialise (const tracktion::PluginInitialisationInfo& info)
{
    stage.prepare (info.sampleRate);
}

void GainStagePlugin::applyToBuffer (const tracktion::PluginRenderContext& context)
{
    auto* buffer = context.destBuffer;

    if (buffer == nullptr || buffer->getNumChannels() < 1)
        return;

    const auto start = context.bufferStartSample;
    const auto count = context.bufferNumSamples;
    stage.process (buffer->getWritePointer (0, start), buffer->getNumChannels() > 1 ? buffer->getWritePointer (1, start) : nullptr, count);

    // A track is stereo (SPEC §4.4). Anything past the second channel is
    // silenced, so a muted track can't be heard through it.
    for (int channel = 2; channel < buffer->getNumChannels(); ++channel)
        buffer->clear (channel, start, count);
}
}
