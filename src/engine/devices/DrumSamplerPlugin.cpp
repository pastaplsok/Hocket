#include "engine/devices/DrumSamplerPlugin.h"

#include <stdexcept>
#include <utility>

namespace hocket::engine
{
namespace
{
    // How often kits no hit plays from any more are freed.
    constexpr int collectIntervalMs = 250;
}

void DrumSamplerPlugin::registerWith (tracktion::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<DrumSamplerPlugin>();
}

juce::ReferenceCountedObjectPtr<DrumSamplerPlugin> DrumSamplerPlugin::create (tracktion::Edit& edit)
{
    juce::ReferenceCountedObjectPtr<DrumSamplerPlugin> plugin (dynamic_cast<DrumSamplerPlugin*> (edit.getPluginCache().createNewPlugin (xmlTypeName, {}).get()));

    if (plugin == nullptr)
        throw std::runtime_error ("Tracktion couldn't create Hocket's drum sampler: is its type registered with the engine?");

    return plugin;
}

DrumSamplerPlugin::DrumSamplerPlugin (tracktion::PluginCreationInfo info) : Plugin (info)
{
    startTimer (collectIntervalMs);
}

DrumSamplerPlugin::~DrumSamplerPlugin()
{
    stopTimer();
    notifyListenersOfDeletion();
}

//==============================================================================
void DrumSamplerPlugin::setKit (KitLibrary* newLibrary, std::string newKit)
{
    if (newLibrary == library && newKit == kit)
        return;

    library = newLibrary;
    kit = std::move (newKit);

    // At the rate it last played at; before it has played, the device's,
    // which is what it will play at, and what a render renders at.
    subscribe (subscribedRate > 0.0 ? subscribedRate : engine.getDeviceManager().getSampleRate());
}

void DrumSamplerPlugin::subscribe (double rate)
{
    subscribedRate = rate;
    subscription = {}; // the last request, and any delivery still to come for it, goes

    if (library == nullptr || kit.empty())
        return;

    subscription = library->subscribe (kit, rate, [this, rate] (std::unique_ptr<PlayableKit> loaded)
    {
        // Delivered on the message thread, each time the kit (re)loads.
        if (loaded != nullptr && loaded->sampleRate() == rate)
            drumVoices.setKit (std::move (loaded));
    });
}

void DrumSamplerPlugin::initialise (const tracktion::PluginInitialisationInfo& info)
{
    if (info.sampleRate == subscribedRate)
        return;

    // A kit at another rate would play at the wrong pitch: silence, until the
    // kit has loaded at this one.
    drumVoices.setKit (PlayableKit::silent (info.sampleRate));
    drumVoices.requestSilence();
    subscribe (info.sampleRate);
}

void DrumSamplerPlugin::reset()
{
    drumVoices.requestSilence();
}

void DrumSamplerPlugin::midiPanic()
{
    drumVoices.requestSilence();
}

void DrumSamplerPlugin::timerCallback()
{
    drumVoices.collectRetired();
}

//==============================================================================
void DrumSamplerPlugin::applyToBuffer (const tracktion::PluginRenderContext& context)
{
    auto* buffer = context.destBuffer;

    if (buffer == nullptr)
        return;

    const auto start = context.bufferStartSample;
    const auto count = context.bufferNumSamples;

    // A synth: what comes in is replaced, and channels past the second stay silent.
    buffer->clear (start, count);

    if (count <= 0 || buffer->getNumChannels() < 2)
        return;

    // Each note-on and all-sound-off, at its sample in the block. A timestamp
    // is in seconds from the buffer's first sample, read as 4OSC reads it, and
    // one that rounds to the block's end plays on its last sample.
    std::size_t numEvents = 0;

    if (const auto* midi = context.bufferForMidiMessages)
    {
        for (const auto& message : *midi)
        {
            if (numEvents == events.size())
                break;

            const auto at = juce::jlimit (start, start + count - 1, juce::roundToInt (message.getTimeStamp() * sampleRate)) - start;

            if (message.isNoteOn())
                events[numEvents++] = { DrumEvent::Kind::hit, at, message.getNoteNumber(), static_cast<int> (message.getVelocity()) };
            else if (message.isAllSoundOff())
                events[numEvents++] = { DrumEvent::Kind::silence, at, 0, 0 };

            // Note-offs and all-notes-off are ignored: one-shots ring on.
        }
    }

    // In order of offset, events at one offset in the order they came: an
    // insertion sort, which allocates nothing, on what's nearly always sorted.
    for (std::size_t i = 1; i < numEvents; ++i)
    {
        const auto event = events[i];
        auto j = i;

        for (; j > 0 && events[j - 1].offset > event.offset; --j)
            events[j] = events[j - 1];

        events[j] = event;
    }

    drumVoices.process (buffer->getWritePointer (0, start), buffer->getWritePointer (1, start), count, std::span<const DrumEvent> (events.data(), numEvents));
}
}
