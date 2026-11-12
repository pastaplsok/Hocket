#pragma once

// The audio device that hocket-headless --play plays on: opening the one asked
// for, what --list-devices says about the devices there are, and what --play
// says about its device when it starts and when it stops.
//
// The device manager can replace its device during a run: when the one in use
// disappears (a USB interface unplugged, say), it closes it, which deletes it,
// and opens another. So nothing here keeps a pointer to a device: each call
// asks the manager for the one open now.

#include <juce_audio_devices/juce_audio_devices.h>

#include <string>
#include <vector>

namespace hocket::engine
{
    // What --play asks for. Empty names and zeros keep what's open.
    struct DeviceChoice
    {
        std::string driver; // a driver type, as --list-devices prints it, e.g. "ASIO"
        std::string device; // an output device, as --list-devices prints it
        int sampleRate = 0; // Hz
        int bufferSize = 0; // samples
    };

    // Switches to the driver type and output device asked for, by the names
    // --list-devices prints: with only a driver, its default device; with only
    // a device, the first driver that lists it; with neither, it keeps what's
    // open. Then sets the sample rate and buffer size asked for, if the device
    // offers them; one it doesn't offer changes nothing. Tracktion saves what's
    // open for the next run. Returns an error, or an empty string.
    std::string openDevice (juce::AudioDeviceManager& devices, const DeviceChoice& choice);

    struct DeviceList
    {
        std::string text;  // what --list-devices prints
        std::string error; // or why it can't
    };

    // Every driver type with its output and input devices, by name. With a
    // driver or device named, only those, and for each of their output devices,
    // the sample rates and buffer sizes it offers. Finding those out loads the
    // device's driver (without starting it), which could upset another program
    // using the same ASIO driver, so it's only done for the devices named.
    DeviceList listDevices (juce::AudioDeviceManager& devices, const std::string& driver, const std::string& device);

    // Sorted, with runs of four or more evenly spaced values written as a range:
    // "64 to 2048 in steps of 32", "44100, 48000". "none" when there are none.
    std::string formatNumbers (std::vector<int> values);

    class PlaybackDevice
    {
    public:
        explicit PlaybackDevice (juce::AudioDeviceManager& devices);

        // "Windows Audio: Speakers, 48000 Hz, 480 samples (10.0 ms)"; empty if no device is open.
        std::string description() const;

        // The xruns since the device opened, for the end of a run: the driver's
        // count where it keeps one (many don't: WASAPI, ASIO4ALL and FL Studio
        // ASIO among them), and JUCE's count of audio callbacks that took longer
        // than their block.
        // "the driver reported 0 xruns, and no audio callback took longer than its block"
        std::string xrunReport() const;

    private:
        juce::AudioDeviceManager& devices;
    };
}
