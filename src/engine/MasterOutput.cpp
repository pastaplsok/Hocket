#include "engine/MasterOutput.h"

#include "core/units/Units.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>

namespace hocket::engine
{
// Written by the audio thread alone, read by the message thread.
struct MasterOutput::Meter
{
    std::atomic<float> peak { 0.0f };
    std::atomic<std::uint64_t> clipped { 0 };

    static_assert (std::atomic<float>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);
};

namespace
{
    // Tracktion's DeviceManager calls processBlock on the audio thread with
    // every output channel, after the Edits have played into them. Nothing
    // here locks or allocates.
    class Processor final : public juce::AudioProcessor
    {
    public:
        Processor (float gainToApply, std::shared_ptr<MasterOutput::Meter> meterToFill)
            : gain (gainToApply), meter (std::move (meterToFill))
        {
        }

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            if (gain == 0.0f)
                buffer.clear();
            else
                buffer.applyGain (gain);

            const auto numSamples = buffer.getNumSamples();
            float blockPeak = 0.0f;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                if (const auto* samples = buffer.getReadPointer (channel); samples != nullptr)
                {
                    const auto range = juce::FloatVectorOperations::findMinAndMax (samples, numSamples);
                    blockPeak = std::max ({ blockPeak, -range.getStart(), range.getEnd() });
                }
            }

            if (blockPeak > 1.0f)
            {
                std::uint64_t over = 0;

                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    if (const auto* samples = buffer.getReadPointer (channel); samples != nullptr)
                        for (int i = 0; i < numSamples; ++i)
                            over += std::abs (samples[i]) > 1.0f ? 1 : 0;

                meter->clipped.fetch_add (over, std::memory_order_relaxed);
            }

            // The only writer, so no compare-and-swap is needed.
            if (blockPeak > meter->peak.load (std::memory_order_relaxed))
                meter->peak.store (blockPeak, std::memory_order_relaxed);
        }

        const juce::String getName() const override { return "Hocket master level"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        const float gain;
        const std::shared_ptr<MasterOutput::Meter> meter;
    };

    // "-23.1 dBFS", "+2.3 dBFS", "0.0 dBFS".
    std::string dbfs (float level)
    {
        auto db = std::round (static_cast<double> (juce::Decibels::gainToDecibels (level, -1000.0f)) * 10.0) / 10.0;

        if (db == 0.0)
            db = 0.0; // not "-0.0"

        char buffer[32];
        const auto [end, error] = std::to_chars (buffer, buffer + sizeof (buffer), db, std::chars_format::fixed, 1);
        const std::string number (buffer, error == std::errc() ? end : buffer);
        return (db > 0.0 ? "+" : "") + number + " dBFS";
    }
}

MasterOutput::MasterOutput (tracktion::DeviceManager& deviceManager, double volume)
    : devices (deviceManager), volumeDb (volume), meter (std::make_shared<Meter>())
{
    const auto gain = std::isinf (volume) ? 0.0f : static_cast<float> (juce::Decibels::decibelsToGain (volume, -1000.0));
    devices.setGlobalOutputAudioProcessor (std::make_unique<Processor> (gain, meter));
}

MasterOutput::~MasterOutput()
{
    // Swapped out under the device's callback lock, so the audio thread is done with it.
    devices.setGlobalOutputAudioProcessor (nullptr);
}

float MasterOutput::peak() const
{
    return meter->peak.load (std::memory_order_relaxed);
}

std::uint64_t MasterOutput::clippedSamples() const
{
    return meter->clipped.load (std::memory_order_relaxed);
}

//==============================================================================
std::string describePeak (float peak)
{
    return peak > 0.0f ? "peak output level " + dbfs (peak) : std::string ("the output was silent");
}

std::string clippingWarning (float peak, std::uint64_t clippedSamples, double volume)
{
    if (clippedSamples == 0 || peak <= 1.0f || std::isinf (volume))
        return {};

    // Whole dB, leaving the peak at -1 dBFS or below.
    const auto& quantity = units::volume();
    const auto suggested = std::max (quantity.min, std::floor (volume - static_cast<double> (juce::Decibels::gainToDecibels (peak)) - 1.0));

    return "Warning: the output clipped: " + std::to_string (clippedSamples) + (clippedSamples == 1 ? " sample went" : " samples went")
         + " over full scale (the peak was " + dbfs (peak) + "), and the driver cuts those off, which distorts the sound. --master-volume "
         + units::format (suggested, quantity) + " would have kept the peak under full scale.";
}
}
