#include "engine/PlaybackDevice.h"

#include <algorithm>

namespace hocket::engine
{
namespace
{
    juce::String juceString (const std::string& utf8)
    {
        return juce::String::fromUTF8 (utf8.data(), static_cast<int> (utf8.size()));
    }

    std::string quotedList (const juce::StringArray& names)
    {
        std::string list;

        for (const auto& name : names)
            list += (list.empty() ? "'" : ", '") + name.toStdString() + "'";

        return list.empty() ? "(none)" : list;
    }
}

std::string openDevice (juce::AudioDeviceManager& devices, const std::string& driver, const std::string& device)
{
    if (driver.empty() && device.empty())
        return {};

    juce::AudioIODeviceType* type = nullptr;
    juce::StringArray typeNames;

    for (auto* candidate : devices.getAvailableDeviceTypes())
    {
        candidate->scanForDevices();
        typeNames.add (candidate->getTypeName());

        const auto matches = driver.empty() ? candidate->getDeviceNames (false).contains (juceString (device))
                                            : candidate->getTypeName() == juceString (driver);
        if (matches && type == nullptr)
            type = candidate;
    }

    if (type == nullptr)
        return driver.empty() ? "no driver has an output device called '" + device + "'; --list-devices lists them"
                              : "there's no driver called '" + driver + "'; the drivers are " + quotedList (typeNames);

    const auto deviceNames = type->getDeviceNames (false);

    if (deviceNames.isEmpty())
        return type->getTypeName().toStdString() + " has no output devices; --list-devices lists the drivers and their devices";

    const auto defaultIndex = std::max (0, type->getDefaultDeviceIndex (false));
    const auto deviceName = device.empty() ? deviceNames[defaultIndex] : juceString (device);

    if (! deviceNames.contains (deviceName))
        return type->getTypeName().toStdString() + " has no output device called '" + device + "'; its output devices are "
             + quotedList (deviceNames);

    if (devices.getCurrentAudioDeviceType() != type->getTypeName())
        devices.setCurrentAudioDeviceType (type->getTypeName(), true);

    auto setup = devices.getAudioDeviceSetup();

    if (setup.outputDeviceName != deviceName || devices.getCurrentAudioDevice() == nullptr)
    {
        setup.outputDeviceName = deviceName;
        setup.inputDeviceName = {};
        setup.useDefaultOutputChannels = true;

        if (const auto error = devices.setAudioDeviceSetup (setup, true); error.isNotEmpty())
            return "couldn't open " + type->getTypeName().toStdString() + " device '" + deviceName.toStdString() + "': " + error.toStdString();
    }

    return {};
}

//==============================================================================
PlaybackDevice::PlaybackDevice (juce::AudioDeviceManager& deviceManager)
    : devices (deviceManager)
{
}

std::string PlaybackDevice::description() const
{
    auto* device = devices.getCurrentAudioDevice();

    if (device == nullptr)
        return {};

    const auto sampleRate = device->getCurrentSampleRate();
    const auto bufferSize = device->getCurrentBufferSizeSamples();

    return device->getTypeName().toStdString() + ": " + device->getName().toStdString() + ", " + juce::String (sampleRate, 0).toStdString()
         + " Hz, " + std::to_string (bufferSize) + " samples (" + juce::String (1000.0 * bufferSize / sampleRate, 1).toStdString() + " ms)";
}

std::string PlaybackDevice::xrunReport() const
{
    auto* device = devices.getCurrentAudioDevice();

    if (device == nullptr)
        return "no audio device is open now, so there's no xrun count";

    // The manager's count is the driver's (when it reports one) plus its own of
    // late callbacks, so the late ones are what's left over.
    const auto driverXruns = device->getXRunCount();
    const auto late = devices.getXRunCount() - std::max (0, driverXruns);

    const auto driver = driverXruns < 0 ? std::string ("this driver doesn't report xruns")
                                        : "the driver reported " + std::to_string (driverXruns) + (driverXruns == 1 ? " xrun" : " xruns");

    const auto callbacks = late == 0 ? std::string ("no audio callback took longer than its block")
                                     : std::to_string (late) + (late == 1 ? " audio callback took longer than its block"
                                                                          : " audio callbacks took longer than their block");

    return driver + ", and " + callbacks;
}
}
