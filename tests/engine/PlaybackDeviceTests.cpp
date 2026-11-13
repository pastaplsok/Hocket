#include "engine/PlaybackDevice.h"

#include <doctest/doctest.h>

#include <utility>
#include <vector>

using namespace hocket;

namespace
{
    class TestDeviceType;

    // What a test device is like.
    struct Offered
    {
        juce::String name;
        int xruns = 0; // what it reports; -1: it doesn't report any, like WASAPI
        juce::Array<double> sampleRates { 44100.0 };
        juce::Array<int> bufferSizes { 256 };
        bool followsRequests = true; // false: it opens at its first rate and size, whatever it's asked for
        juce::String failsToOpen;    // the error it opens with, if any
    };

    // A device that never makes a sound, and plays a block when a test asks.
    class TestDevice final : public juce::AudioIODevice
    {
    public:
        TestDevice (const Offered& offeredAs, std::vector<juce::String>& deletedNames)
            : AudioIODevice (offeredAs.name, "Test"), offered (offeredAs), deleted (deletedNames)
        {
        }

        ~TestDevice() override
        {
            close();
            deleted.push_back (getName());
        }

        juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
        juce::StringArray getInputChannelNames() override { return {}; }
        juce::Array<double> getAvailableSampleRates() override { return offered.sampleRates; }
        juce::Array<int> getAvailableBufferSizes() override { return offered.bufferSizes; }
        int getDefaultBufferSize() override { return offered.bufferSizes.getFirst(); }

        juce::String open (const juce::BigInteger&, const juce::BigInteger& outputs, double rate, int size) override
        {
            if (offered.failsToOpen.isNotEmpty())
                return offered.failsToOpen;

            opened = true;
            activeOutputs = outputs;

            if (offered.followsRequests && offered.sampleRates.contains (rate))
                sampleRate = rate;

            if (offered.followsRequests && offered.bufferSizes.contains (size))
                blockSize = size;

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
        int getXRunCount() const noexcept override { return offered.xruns; }

        // One block through whatever the device manager has started on it.
        void playBlock()
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();

            if (callback != nullptr)
                callback->audioDeviceIOCallbackWithContext (nullptr, 0, buffer.getArrayOfWritePointers(), 2, blockSize, {});
        }

    private:
        Offered offered;
        std::vector<juce::String>& deleted;
        double sampleRate = offered.sampleRates.getFirst();
        int blockSize = offered.bufferSizes.getFirst();
        bool opened = false;
        juce::BigInteger activeOutputs;
        juce::AudioIODeviceCallback* callback = nullptr;
    };

    // A driver whose devices a test can unplug. The first device is the default.
    class TestDeviceType final : public juce::AudioIODeviceType
    {
    public:
        explicit TestDeviceType (std::vector<Offered> devices)
            : AudioIODeviceType ("Test"), offered (std::move (devices))
        {
        }

        void scanForDevices() override {}

        juce::StringArray getDeviceNames (bool wantInputNames) const override
        {
            juce::StringArray names;

            if (! wantInputNames)
                for (const auto& device : offered)
                    names.add (device.name);

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
            for (const auto& device : offered)
                if (device.name == outputName)
                    return new TestDevice (device, deletedDevices);

            return nullptr;
        }

        // As when a USB interface is unplugged: the device manager hears of it at once.
        void unplug (const juce::String& name)
        {
            std::erase_if (offered, [&name] (const auto& device) { return device.name == name; });
            callDeviceChangeListeners();
        }

        std::vector<juce::String> deletedDevices; // names, in the order they went

    private:
        std::vector<Offered> offered;
    };

    // Only test devices: with a device type of its own, the manager never
    // creates the real ones (WASAPI, ASIO).
    TestDeviceType& addTestDevices (juce::AudioDeviceManager& devices, std::vector<Offered> offered)
    {
        auto type = std::make_unique<TestDeviceType> (std::move (offered));
        auto& added = *type;
        devices.addAudioDeviceType (std::move (type));
        return added;
    }

    std::string pick (juce::AudioDeviceManager& devices, const std::string& driver, const std::string& device, int sampleRate = 0, int bufferSize = 0)
    {
        return engine::openDevice (devices, { driver, device, sampleRate, bufferSize });
    }

    juce::Array<int> evenSizes (int from, int to, int step)
    {
        juce::Array<int> sizes;

        for (int size = from; size <= to; size += step)
            sizes.add (size);

        return sizes;
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

    REQUIRE (pick (devices, "Test", "Interface").empty());
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
    REQUIRE (pick (devices, "Test", "Speakers").empty());

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

TEST_CASE ("drivers that may bypass the Windows volume slider come with a warning to turn the device down")
{
    const auto warning = engine::volumeWarning ("ASIO");
    CHECK (warning == "Warning: ASIO drivers may bypass the Windows volume slider, so the device's own volume control is what sets the level. "
                      "If it isn't down already, turn it down now, and bring it up once you hear the music.");
    CHECK (engine::volumeWarning ("Windows Audio (Exclusive Mode)").starts_with ("Warning: Windows Audio in exclusive mode may bypass"));

    CHECK (engine::volumeWarning ("Windows Audio").empty());
    CHECK (engine::volumeWarning ("Windows Audio (Low Latency Mode)").empty());
    CHECK (engine::volumeWarning ("DirectSound").empty());

    // The device open now: a test device, so none; and none with no device.
    juce::AudioDeviceManager devices;
    auto& type = addTestDevices (devices, { { "Interface", 0 } });
    REQUIRE (pick (devices, "Test", "Interface").empty());
    const engine::PlaybackDevice playing (devices);
    CHECK (playing.volumeWarning().empty());

    type.unplug ("Interface");
    CHECK (playing.volumeWarning().empty());
}

TEST_CASE ("the xrun report says when no device is open")
{
    juce::AudioDeviceManager devices;
    auto& type = addTestDevices (devices, { { "Interface", 0 } });
    REQUIRE (pick (devices, "Test", "Interface").empty());
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

    CHECK (pick (devices, "ASIO", "") == "there's no driver called 'ASIO'; the drivers are 'Test'");
    CHECK (pick (devices, "", "Headphones") == "no driver has an output device called 'Headphones'; --list-devices lists them");
    CHECK (pick (devices, "Test", "Headphones") == "Test has no output device called 'Headphones'; its output devices are 'Interface', 'Speakers'");

    // A driver alone opens its default device.
    CHECK (pick (devices, "Test", "").empty());
    REQUIRE (devices.getCurrentAudioDevice() != nullptr);
    CHECK (devices.getCurrentAudioDevice()->getName() == "Interface");
}

TEST_CASE ("opening a device says when the driver has none at all")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, {});

    CHECK (pick (devices, "Test", "") == "Test has no output devices; --list-devices lists the drivers and their devices");
    CHECK (devices.getCurrentAudioDevice() == nullptr);
}

TEST_CASE ("opening a device suggests the name meant, when only one contains what was given")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Speakers (Magni Mesh Dac)" }, { "Realtek Speakers" } });

    CHECK (pick (devices, "", "Magni Mesh Dac")
           == "no driver has an output device called 'Magni Mesh Dac'; did you mean 'Speakers (Magni Mesh Dac)'? --list-devices lists them");
    CHECK (pick (devices, "Test", "magni")
           == "Test has no output device called 'magni'; did you mean 'Speakers (Magni Mesh Dac)'? Its output devices are "
              "'Speakers (Magni Mesh Dac)', 'Realtek Speakers'");
    CHECK (pick (devices, "test", "") == "there's no driver called 'test'; did you mean 'Test'? The drivers are 'Test'");

    // Both names contain "Speakers", so neither is suggested.
    CHECK (pick (devices, "", "Speakers") == "no driver has an output device called 'Speakers'; --list-devices lists them");
    CHECK (devices.getCurrentAudioDevice() == nullptr);
}

TEST_CASE ("opening a device sets the sample rate and buffer size asked for")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Interface", 0, { 44100.0, 48000.0 }, { 128, 256, 512 } } });
    const engine::PlaybackDevice playing (devices);

    REQUIRE (pick (devices, "Test", "Interface", 48000, 512) == "");
    CHECK (playing.description() == "Test: Interface, 48000 Hz, 512 samples (10.7 ms)");

    // On the device that's open, either one alone.
    REQUIRE (pick (devices, "", "", 44100) == "");
    CHECK (playing.description() == "Test: Interface, 44100 Hz, 512 samples (11.6 ms)");
    REQUIRE (pick (devices, "", "", 0, 128) == "");
    CHECK (playing.description() == "Test: Interface, 44100 Hz, 128 samples (2.9 ms)");
}

TEST_CASE ("a sample rate or buffer size the device doesn't offer is refused, and changes nothing")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Interface", 0, { 44100.0, 48000.0 }, evenSizes (64, 2048, 32) } });
    const engine::PlaybackDevice playing (devices);
    REQUIRE (pick (devices, "Test", "Interface", 48000, 256) == "");

    CHECK (pick (devices, "", "", 96000) == "Test device 'Interface' doesn't offer 96000 Hz; it offers 44100, 48000 Hz");
    CHECK (pick (devices, "", "", 0, 100)
           == "Test device 'Interface' doesn't offer a buffer of 100 samples; it offers 64 to 2048 in steps of 32 samples");
    CHECK (playing.description() == "Test: Interface, 48000 Hz, 256 samples (5.3 ms)");
}

TEST_CASE ("a driver that carries on as it was, though it offers what was asked for, is caught")
{
    juce::AudioDeviceManager devices;
    Offered stubborn { "Interface", 0, { 44100.0, 48000.0 }, { 256, 512 } };
    stubborn.followsRequests = false;
    addTestDevices (devices, { stubborn });

    CHECK (pick (devices, "Test", "Interface", 48000)
           == "Test device 'Interface' is running at 44100 Hz: its driver didn't switch to the 48000 Hz asked for");
    CHECK (pick (devices, "", "", 0, 512) == "Test device 'Interface' is using a buffer of 256 samples: its driver didn't switch to the 512 asked for");
}

TEST_CASE ("a device whose driver won't start says what to try")
{
    juce::AudioDeviceManager devices;
    Offered broken { "Interface" };
    broken.failsToOpen = "Device didn't start correctly";
    addTestDevices (devices, { { "Speakers" }, broken });

    CHECK (pick (devices, "Test", "Interface")
           == "couldn't open Test device 'Interface': Device didn't start correctly. Its driver may be set to an output that's missing or in use "
              "by another program: check the driver's own settings, or choose another device (--list-devices --driver Test lists them)");

    // Another of its devices still opens.
    CHECK (pick (devices, "Test", "Speakers").empty());
}

TEST_CASE ("with no device open, there's nothing to set the sample rate of")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Interface" } });

    CHECK (pick (devices, "", "", 48000)
           == "no audio device is open to set the sample rate or buffer size of; choose one with --driver and --device (--list-devices lists them)");
}

TEST_CASE ("lists of numbers are written with evenly spaced runs as ranges")
{
    CHECK (engine::formatNumbers ({}) == "none");
    CHECK (engine::formatNumbers ({ 48000, 44100, 48000 }) == "44100, 48000");
    CHECK (engine::formatNumbers ({ 64, 128, 256, 512, 1024 }) == "64, 128, 256, 512, 1024");
    CHECK (engine::formatNumbers ({ 1, 2, 3 }) == "1, 2, 3");
    CHECK (engine::formatNumbers ({ 1, 2, 3, 4, 10 }) == "1 to 4 in steps of 1, 10");

    // As JUCE lists an ASIO driver's buffer sizes: evenly spaced, plus the driver's preferred one.
    const auto even = evenSizes (64, 2048, 32);
    std::vector<int> sizes (even.begin(), even.end());
    sizes.push_back (1000);
    CHECK (engine::formatNumbers (sizes) == "64 to 992 in steps of 32, 1000, 1024 to 2048 in steps of 32");
}

TEST_CASE ("the device list names every device, and describes those of a driver or device asked about")
{
    juce::AudioDeviceManager devices;
    addTestDevices (devices, { { "Interface", 0, { 44100.0, 48000.0 }, evenSizes (64, 2048, 32) }, { "Speakers" } });

    const auto plain = engine::listDevices (devices, "", "");
    CHECK (plain.error == "");
    CHECK (plain.text == "Test\n  outputs:\n    Interface\n    Speakers\n  inputs:\n    (none)\n");

    const auto driver = engine::listDevices (devices, "Test", "");
    CHECK (driver.error == "");
    CHECK (driver.text
           == "Test\n"
              "  outputs:\n"
              "    Interface\n"
              "      sample rates: 44100, 48000 Hz (now 44100)\n"
              "      buffer sizes: 64 to 2048 in steps of 32 samples (default 64)\n"
              "    Speakers\n"
              "      sample rates: 44100 Hz (now 44100)\n"
              "      buffer sizes: 256 samples (default 256)\n");

    const auto device = engine::listDevices (devices, "", "Speakers");
    CHECK (device.text == "Test\n  outputs:\n    Speakers\n      sample rates: 44100 Hz (now 44100)\n      buffer sizes: 256 samples (default 256)\n");

    CHECK (engine::listDevices (devices, "ASIO", "").error == "there's no driver called 'ASIO'; the drivers are 'Test'");
    CHECK (engine::listDevices (devices, "Test", "Inter").error
           == "Test has no output device called 'Inter'; did you mean 'Interface'? Its output devices are 'Interface', 'Speakers'");
    CHECK (engine::listDevices (devices, "", "Phones").error == "no driver has an output device called 'Phones'; --list-devices lists them");

    // Asking what a device offers doesn't open it.
    CHECK (devices.getCurrentAudioDevice() == nullptr);
}
