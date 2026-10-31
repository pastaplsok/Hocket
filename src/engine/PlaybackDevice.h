#pragma once

// The audio device that hocket-headless --play plays on: opening the one asked
// for, and what --play says about it when it starts and when it stops.
//
// The device manager can replace its device during a run: when the one in use
// disappears (a USB interface unplugged, say), it closes it, which deletes it,
// and opens another. So nothing here keeps a pointer to a device: each call
// asks the manager for the one open now.

#include <juce_audio_devices/juce_audio_devices.h>

#include <string>

namespace hocket::engine
{
    // Switches to the driver type and output device asked for, by the names
    // --list-devices prints. With only a driver, its default device; with only
    // a device, the first driver that lists it; with neither, it keeps what's
    // open. Returns an error, or an empty string.
    std::string openDevice (juce::AudioDeviceManager& devices, const std::string& driver, const std::string& device);

    class PlaybackDevice
    {
    public:
        explicit PlaybackDevice (juce::AudioDeviceManager& devices);

        // "Windows Audio: Speakers, 48000 Hz, 480 samples (10.0 ms)"; empty if no device is open.
        std::string description() const;

        // The xruns since the device opened, for the end of a run: the driver's
        // count where it keeps one (ASIO drivers usually do, WASAPI doesn't), and
        // JUCE's count of audio callbacks that took longer than their block.
        // "the driver reported 0 xruns, and no audio callback took longer than its block"
        std::string xrunReport() const;

    private:
        juce::AudioDeviceManager& devices;
    };
}
