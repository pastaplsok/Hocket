#include "engine/PlaybackDevice.h"

#include <algorithm>
#include <memory>
#include <string_view>

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

    // "did you mean 'Speakers (Magni Mesh Dac)'? " when exactly one name
    // contains what was given, ignoring case; otherwise empty.
    std::string didYouMean (const juce::StringArray& names, const std::string& given)
    {
        const auto text = juceString (given);
        juce::String found;

        for (const auto& name : names)
        {
            if (! name.containsIgnoreCase (text))
                continue;

            if (found.isNotEmpty() && found != name)
                return {};

            found = name;
        }

        return found.isEmpty() ? std::string() : "did you mean '" + found.toStdString() + "'? ";
    }

    // Each driver type, with its list of devices brought up to date.
    std::vector<juce::AudioIODeviceType*> scannedTypes (juce::AudioDeviceManager& devices)
    {
        std::vector<juce::AudioIODeviceType*> types;

        for (auto* type : devices.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            types.push_back (type);
        }

        return types;
    }

    // The driver type called `driver`, or nullptr and an error naming the ones there are.
    juce::AudioIODeviceType* findDriver (const std::vector<juce::AudioIODeviceType*>& types, const std::string& driver, std::string& error)
    {
        juce::StringArray names;

        for (auto* type : types)
        {
            if (type->getTypeName() == juceString (driver))
                return type;

            names.add (type->getTypeName());
        }

        const auto hint = didYouMean (names, driver);
        error = "there's no driver called '" + driver + "'; " + (hint.empty() ? "the" : hint + "The") + " drivers are " + quotedList (names);
        return nullptr;
    }

    std::string noSuchDevice (const juce::AudioIODeviceType& type, const std::string& device)
    {
        const auto outputs = type.getDeviceNames (false);
        const auto hint = didYouMean (outputs, device);

        return type.getTypeName().toStdString() + " has no output device called '" + device + "'; " + (hint.empty() ? "its" : hint + "Its")
             + " output devices are " + quotedList (outputs);
    }

    std::string noDriverHas (const std::vector<juce::AudioIODeviceType*>& types, const std::string& device)
    {
        juce::StringArray outputs;

        for (auto* type : types)
            outputs.addArray (type->getDeviceNames (false));

        return "no driver has an output device called '" + device + "'; " + didYouMean (outputs, device) + "--list-devices lists them";
    }

    // A driver's own error says what failed ("Device didn't start correctly",
    // as FL Studio ASIO does), not what to try.
    std::string whatToTry (const juce::AudioIODeviceType& type, const juce::String& error)
    {
        const auto name = type.getTypeName().toStdString();
        const auto driver = type.getTypeName().containsChar (' ') ? "\"" + name + "\"" : name;

        return error.trim().trimCharactersAtEnd (".").toStdString()
             + ". Its driver may be set to an output that's missing or in use by another program: check the driver's own settings, "
               "or choose another device (--list-devices --driver " + driver + " lists them)";
    }

    // Opens the output device asked for, keeping the sample rate and buffer size if it can.
    std::string switchDevice (juce::AudioDeviceManager& devices, const std::string& driver, const std::string& device)
    {
        const auto types = scannedTypes (devices);
        juce::AudioIODeviceType* type = nullptr;

        if (! driver.empty())
        {
            std::string error;
            type = findDriver (types, driver, error);

            if (type == nullptr)
                return error;
        }
        else
        {
            const auto lists = [&device] (auto* candidate) { return candidate->getDeviceNames (false).contains (juceString (device)); };

            if (const auto found = std::ranges::find_if (types, lists); found != types.end())
                type = *found;
            else
                return noDriverHas (types, device);
        }

        const auto deviceNames = type->getDeviceNames (false);

        if (deviceNames.isEmpty())
            return type->getTypeName().toStdString() + " has no output devices; --list-devices lists the drivers and their devices";

        const auto defaultIndex = std::max (0, type->getDefaultDeviceIndex (false));
        const auto deviceName = device.empty() ? deviceNames[defaultIndex] : juceString (device);

        if (! deviceNames.contains (deviceName))
            return noSuchDevice (*type, device);

        if (devices.getCurrentAudioDeviceType() != type->getTypeName())
            devices.setCurrentAudioDeviceType (type->getTypeName(), true);

        auto setup = devices.getAudioDeviceSetup();

        if (setup.outputDeviceName != deviceName || devices.getCurrentAudioDevice() == nullptr)
        {
            setup.outputDeviceName = deviceName;
            setup.inputDeviceName = {};
            setup.useDefaultOutputChannels = true;

            if (const auto error = devices.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                return "couldn't open " + type->getTypeName().toStdString() + " device '" + deviceName.toStdString() + "': " + whatToTry (*type, error);
        }

        return {};
    }

    std::vector<int> wholeNumbers (const juce::Array<double>& values)
    {
        std::vector<int> whole;

        for (const auto value : values)
            whole.push_back (juce::roundToInt (value));

        return whole;
    }

    std::vector<int> toVector (const juce::Array<int>& values)
    {
        return { values.begin(), values.end() };
    }

    bool contains (const std::vector<int>& values, int value)
    {
        return std::ranges::find (values, value) != values.end();
    }

    // "it offers 44100, 48000 Hz", or "it reports none".
    std::string offers (const std::vector<int>& values, std::string_view unit)
    {
        return values.empty() ? std::string ("it reports none") : "it offers " + formatNumbers (values) + " " + std::string (unit);
    }

    // Checks the open device offers them before asking for them: the device
    // manager would otherwise take the nearest it does offer, and Tracktion
    // would save that for the next run.
    std::string setRateAndSize (juce::AudioDeviceManager& devices, int sampleRate, int bufferSize)
    {
        auto* device = devices.getCurrentAudioDevice();

        if (device == nullptr)
            return "no audio device is open to set the sample rate or buffer size of; choose one with --driver and --device (--list-devices lists them)";

        const auto label = device->getTypeName().toStdString() + " device '" + device->getName().toStdString() + "'";
        const auto rates = wholeNumbers (device->getAvailableSampleRates());
        const auto sizes = toVector (device->getAvailableBufferSizes());

        if (sampleRate > 0 && ! contains (rates, sampleRate))
            return label + " doesn't offer " + std::to_string (sampleRate) + " Hz; " + offers (rates, "Hz");

        if (bufferSize > 0 && ! contains (sizes, bufferSize))
            return label + " doesn't offer a buffer of " + std::to_string (bufferSize) + " samples; " + offers (sizes, "samples");

        const auto current = devices.getAudioDeviceSetup();
        auto setup = current;

        if (sampleRate > 0)
            setup.sampleRate = sampleRate;

        if (bufferSize > 0)
            setup.bufferSize = bufferSize;

        if (! (setup == current))
            if (const auto error = devices.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                return "couldn't reopen " + label + " with the sample rate and buffer size asked for: " + error.toStdString();

        // A driver can say yes and carry on as it was.
        device = devices.getCurrentAudioDevice();

        if (device == nullptr)
            return "couldn't reopen " + label + " with the sample rate and buffer size asked for";

        if (const auto rate = juce::roundToInt (device->getCurrentSampleRate()); sampleRate > 0 && rate != sampleRate)
            return label + " is running at " + std::to_string (rate) + " Hz: its driver didn't switch to the " + std::to_string (sampleRate) + " Hz asked for";

        if (const auto size = device->getCurrentBufferSizeSamples(); bufferSize > 0 && size != bufferSize)
            return label + " is using a buffer of " + std::to_string (size) + " samples: its driver didn't switch to the " + std::to_string (bufferSize)
                 + " asked for";

        return {};
    }

    std::string nameList (std::string_view label, const juce::StringArray& names)
    {
        auto text = "  " + std::string (label) + ":\n";

        if (names.isEmpty())
            text += "    (none)\n";

        for (const auto& name : names)
            text += "    " + name.toStdString() + "\n";

        return text;
    }

    // The sample rates and buffer sizes an output device offers. Creating the
    // device loads its driver, but doesn't start it.
    std::string describeOptions (juce::AudioIODeviceType& type, const juce::String& name)
    {
        const std::unique_ptr<juce::AudioIODevice> device (type.createDevice (name, {}));

        if (device == nullptr)
            return "      its driver couldn't be loaded\n";

        const auto rates = wholeNumbers (device->getAvailableSampleRates());
        const auto sizes = toVector (device->getAvailableBufferSizes());

        if (rates.empty() && sizes.empty())
        {
            const auto error = device->getLastError().toStdString();
            return "      its driver reports no sample rates or buffer sizes" + (error.empty() ? "" : ": " + error) + "\n";
        }

        auto text = "      sample rates: " + formatNumbers (rates) + (rates.empty() ? "" : " Hz");

        if (const auto now = juce::roundToInt (device->getCurrentSampleRate()); now > 0)
            text += " (now " + std::to_string (now) + ")";

        text += "\n      buffer sizes: " + formatNumbers (sizes) + (sizes.empty() ? "" : " samples");

        if (const auto preferred = device->getDefaultBufferSize(); preferred > 0)
            text += " (default " + std::to_string (preferred) + ")";

        return text + "\n";
    }
}

std::string openDevice (juce::AudioDeviceManager& devices, const DeviceChoice& choice)
{
    if (! choice.driver.empty() || ! choice.device.empty())
        if (auto error = switchDevice (devices, choice.driver, choice.device); ! error.empty())
            return error;

    if (choice.sampleRate > 0 || choice.bufferSize > 0)
        return setRateAndSize (devices, choice.sampleRate, choice.bufferSize);

    return {};
}

DeviceList listDevices (juce::AudioDeviceManager& devices, const std::string& driver, const std::string& device)
{
    const auto types = scannedTypes (devices);
    auto shown = types;
    juce::AudioIODeviceType* driverType = nullptr;

    if (! driver.empty())
    {
        std::string error;
        driverType = findDriver (types, driver, error);

        if (driverType == nullptr)
            return { {}, error };

        shown = { driverType };
    }

    if (! device.empty())
    {
        std::erase_if (shown, [&device] (auto* type) { return ! type->getDeviceNames (false).contains (juceString (device)); });

        if (shown.empty())
            return { {}, driverType != nullptr ? noSuchDevice (*driverType, device) : noDriverHas (types, device) };
    }

    const auto detailed = driverType != nullptr || ! device.empty();
    std::string text;

    for (auto* type : shown)
    {
        text += type->getTypeName().toStdString() + "\n";
        const auto outputs = type->getDeviceNames (false);

        if (! detailed)
        {
            text += nameList ("outputs", outputs) + nameList ("inputs", type->getDeviceNames (true));
            continue;
        }

        text += "  outputs:\n";

        if (outputs.isEmpty())
            text += "    (none)\n";

        for (const auto& name : outputs)
            if (device.empty() || name == juceString (device))
                text += "    " + name.toStdString() + "\n" + describeOptions (*type, name);
    }

    return { text, {} };
}

std::string formatNumbers (std::vector<int> values)
{
    std::ranges::sort (values);
    values.erase (std::unique (values.begin(), values.end()), values.end());

    if (values.empty())
        return "none";

    std::string text;
    const auto add = [&text] (const std::string& part) { text += (text.empty() ? "" : ", ") + part; };

    for (std::size_t start = 0; start < values.size();)
    {
        // One past the end of the evenly spaced run that starts here.
        auto end = start + 1;

        if (end < values.size())
        {
            const auto step = values[end] - values[start];

            while (end + 1 < values.size() && values[end + 1] - values[end] == step)
                ++end;

            ++end;
        }

        if (end - start >= 4)
        {
            add (std::to_string (values[start]) + " to " + std::to_string (values[end - 1]) + " in steps of " + std::to_string (values[start + 1] - values[start]));
            start = end;
        }
        else
        {
            add (std::to_string (values[start]));
            ++start;
        }
    }

    return text;
}

//==============================================================================
std::string volumeWarning (const juce::String& driverType)
{
    // ASIO drivers talk to the device directly, and exclusive mode takes it
    // from Windows' mixer, so the volume slider may not apply to either.
    const auto drivers = driverType == "ASIO" ? std::string ("ASIO drivers")
                       : driverType.containsIgnoreCase ("Exclusive") ? "Windows Audio in exclusive mode"
                                                                     : std::string();

    if (drivers.empty())
        return {};

    return "Warning: " + drivers + " may bypass the Windows volume slider, so the device's own volume control is what sets the level. "
           "If it isn't down already, turn it down now, and bring it up once you hear the music.";
}

PlaybackDevice::PlaybackDevice (juce::AudioDeviceManager& deviceManager)
    : devices (deviceManager)
{
}

std::string PlaybackDevice::volumeWarning() const
{
    const auto* device = devices.getCurrentAudioDevice();
    return device == nullptr ? std::string() : engine::volumeWarning (device->getTypeName());
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
