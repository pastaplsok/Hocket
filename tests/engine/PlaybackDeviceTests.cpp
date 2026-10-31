#include "engine/PlaybackDevice.h"

#include <doctest/doctest.h>

#include <utility>
#include <vector>

using namespace hocket;

namespace
{
    class TestDeviceType;

    // A device that never makes a sound. It reports the xruns it's given (-1:
    // it doesn't report any, like WASAPI), and plays a block when a test asks.
    class TestDevice final : public juce::AudioIODevice
    {
    public:
        TestDevice (const juce::String& deviceName, int xrunsToReport, std::vector<juce::String>& deletedNames)
            : AudioIODevice (deviceName, "Test"), xruns (xrunsToReport), deleted (deletedNames)
        {
        }

        ~TestDevice() override
        {
            close();
            deleted.push_back (getName());
        }

        juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
        juce::StringArray getInputChannelNames() override { return {}; }
        juce::Array<double> getAvailableSampleRates() override { return { sampleRate }; }
        juce::Array<int> getAvailableBufferSizes() override { return { blockSize }; }
        int getDefaultBufferSize() override { return blockSize; }

        juce::String open (const juce::BigInteger&, const juce::BigInteger& outputs, double, int) override
        {
            opened = true;
            activeOutputs = outputs;
            return {};
        }

        void close() override
        {
            stop();
            opened = false;
        }

        bool isOpen() override { return opened; }

        void start (juce::AudioIODeviceCallback* newCallback) override
        {
            if (newCallback == nullptr)
                return;

            newCallback->audioDeviceAboutToStart (this);
            callback = newCallback;
        }

        void stop() override
        {
            if (auto* stopping = std::exchange (callback, nullptr))
                stopping->audioDeviceStopped();
        }

        bool isPlaying() override { return callback != nullptr; }
        juce::String getLastError() override { return {}; }
        int getCurrentBufferSizeSamples() override { return blockSize; }
        double getCurrentSampleRate() override { return sampleRate; }
        int getCurrentBitDepth() override { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
        juce::BigInteger getActiveInputChannels() const override { return {}; }
        int getOutputLatencyInSamples() override { return 0; }
        int getInputLatencyInSamples() override { return 0; }
        int getXRunCount() const noexcept override { return xruns; }

        // One block through whatever the device manager has started on it.
        void playBlock()
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();

            if (callback != nullptr)
                callback->audioDeviceIOCallbackWithContext (nullptr, 0, buffer.getArrayOfWritePointers(), 2, blockSize, {});
        }

    private:
        static constexpr double sampleRate = 44100.0;
        static constexpr int blockSize = 256;

        int xruns;
        std::vector<juce::String>& deleted;
        bool opened = false;
        juce::BigInteger activeOutputs;
        juce::AudioIODeviceCallback* callback = nullptr;
    };

    // A driver whose devices a test can unplug. The first device is the default.
    class TestDeviceType final : public juce::AudioIODeviceType
    {
    public:
        explicit TestDeviceType (std::vector<std::pair<juce::String, int>> devicesWithXruns)
            : AudioIODeviceType ("Test"), offered (std::move (devicesWithXruns))
        {
        }

        void scanForDevices() override {}

        juce::StringArray getDeviceNames (bool wantInputNames) const override
        {
            juce::StringArray names;

            if (! wantInputNames)
                for (const auto& device : offered)
                    names.add (device.first);

            return names;
        }

        int getDefaultDeviceIndex (bool forInput) const override { return forInput || offered.empty() ? -1 : 0; }

        int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
        {
            return asInput || device == nullptr ? -1 : getDeviceNames (false).indexOf (device->getName());
        }

        bool hasSeparateInputsAndOutputs() const override { return false; }

        juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String&) override
        {
            for (const auto& [name, xruns] : offered)
                if (name == outputName)
                    return new TestDevice (name, xruns, deletedDevices);

            return nullptr;
        }

        // As when a USB interface is unplugged: the device manager hears of it at once.
        void unplug (const juce::String& name)
        {
            std::erase_if (offered, [&name] (const auto& device) { return device.first == name; });
            callDeviceChangeListeners();
        }

        std::vector<juce::String> deletedDevices; // names, in the order they went

    private:
        std::vector<std::pair<juce::String, int>> offered;
    };

    // Only test devices: with a device type of its own, the manager never
    // creates the real ones (WASAPI, ASIO).
    TestDeviceType& addTestDevices (juce::AudioDeviceManager& devices, std::vector<std::pair<juce::String, int>> devicesWithXruns)
    {
        auto type = std::make_unique<TestDeviceType> (std::move (devicesWithXruns));
        auto& added = *type;
        devices.addAudioDeviceType (std::move (type));
        return added;
    }

    // Takes longer than its block, every time.
    struct SlowCallback final : juce::AudioIODeviceCallback
    {
        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override
        {
            juce::Thread::sleep (20); // a block is 5.8 ms
        }

        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}
    };
}

TEST_CASE ("the playback device's report is about the device open at the time, even after the first was unplugged")
{
    juce::AudioDeviceManager devices;
    auto& type = addTestDevices (devices, { { "Interface", 4 }, { "Speakers", 1 } });

    REQUIRE (engine::openDevice (devices, "Test", "Interface").empty());
    const engine::PlaybackDevice playing (devices);
    CHECK (playing.description() == "Test: Interface, 44100 Hz, 256 samples (5.8 ms)");

    // The device manager deletes the unplugged device and opens the default.
    type.unplug ("Interface");
    REQUIRE (devices.getCurrentAudioDevice() != nullptr);
    REQUIRE (devices.getCurrentAudioDevice()->getName() == "Speakers");
    REQUIRE (type.deletedDevices == std::vector<juce::String> { "Interface" });

    CHECK (playing.description() == "Test: Speakers, 44100 Hz, 256 samples (5.8 ms)");
    CHECK (playing.xrunReport() == "the driver reported 1 xrun, and no audio callback took longer than its block");
}

TEST_CASE ("the xrun report counts audio callbacks that took longer than their block, even when the driver counts none")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Speakers", -1 } });
    REQUIRE (engine::openDevice (devices, "Test", "Speakers").empty());

    const engine::PlaybackDevice playing (devices);
    CHECK (playing.xrunReport() == "this driver doesn't report xruns, and no audio callback took longer than its block");

    SlowCallback slow;
    devices.addAudioCallback (&slow);
    auto* device = dynamic_cast<TestDevice*> (devices.getCurrentAudioDevice());
    REQUIRE (device != nullptr);
    device->playBlock();
    device->playBlock();
    devices.removeAudioCallback (&slow);

    CHECK (playing.xrunReport() == "this driver doesn't report xruns, and 2 audio callbacks took longer than their block");
}

TEST_CASE ("the xrun report says when no device is open")
{
    juce::AudioDeviceManager devices;
    auto& type = addTestDevices (devices, { { "Interface", 0 } });
    REQUIRE (engine::openDevice (devices, "Test", "Interface").empty());
    const engine::PlaybackDevice playing (devices);

    type.unplug ("Interface");
    REQUIRE (devices.getCurrentAudioDevice() == nullptr);

    CHECK (playing.description().empty());
    CHECK (playing.xrunReport() == "no audio device is open now, so there's no xrun count");
}

TEST_CASE ("opening a device names what's there when the one asked for isn't")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Interface", 0 }, { "Speakers", 0 } });

    CHECK (engine::openDevice (devices, "ASIO", "") == "there's no driver called 'ASIO'; the drivers are 'Test'");
    CHECK (engine::openDevice (devices, "", "Headphones") == "no driver has an output device called 'Headphones'; --list-devices lists them");
    CHECK (engine::openDevice (devices, "Test", "Headphones") == "Test has no output device called 'Headphones'; its output devices are 'Interface', 'Speakers'");

    // A driver alone opens its default device.
    CHECK (engine::openDevice (devices, "Test", "").empty());
    REQUIRE (devices.getCurrentAudioDevice() != nullptr);
    CHECK (devices.getCurrentAudioDevice()->getName() == "Interface");
}

TEST_CASE ("opening a device says when the driver has none at all")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, {});

    CHECK (engine::openDevice (devices, "Test", "") == "Test has no output devices; --list-devices lists the drivers and their devices");
    CHECK (devices.getCurrentAudioDevice() == nullptr);
}
