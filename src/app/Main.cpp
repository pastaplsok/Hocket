// hocket-headless: the Milestone 0 command-line host (SPEC §10.0).
//
// It can check a project folder, list audio devices, and play a project on a
// loop. The API for MCP clients comes in a later step.

#include "Console.h"

#include "core/Paths.h"
#include "core/commands/CommandBus.h"
#include "core/project/ClipFileWatcher.h"
#include "core/project/Project.h"
#include "engine/EditSync.h"

#include <tracktion_engine/tracktion_engine.h>

#include <iostream>
#include <optional>
#include <string_view>

namespace
{
    // The author of the commands hocket-headless sends itself. The human's
    // display name becomes a setting with step 6's author identities.
    const std::string localUser = "user";

    // Listing devices must not open one, so Tracktion's device manager isn't
    // initialised on startup.
    struct ListDevicesBehaviour final : tracktion::EngineBehaviour
    {
        bool autoInitialiseDeviceManager() override { return false; }
    };

    // Milestone 0 only plays, so no inputs: opening one can ask for the microphone.
    struct PlayBehaviour final : tracktion::EngineBehaviour
    {
        bool shouldOpenAudioInputByDefault() override { return false; }
    };

    void printUsage (std::ostream& out)
    {
        out << "usage: hocket-headless --check <project-folder>   validate a project; errors go to stderr\n"
               "       hocket-headless --play <project-folder> [--driver <type>] [--device <name>] [--watch]\n"
               "                                                  play the project on a loop until Ctrl+C\n"
               "       hocket-headless --list-devices             list audio devices by driver type\n"
               "       hocket-headless --version                  print version numbers\n"
               "\n"
               "--play options:\n"
               "  --driver <type>   a driver type as --list-devices prints it, e.g. ASIO\n"
               "  --device <name>   an output device as --list-devices prints it, e.g. \"ASIO4ALL v2\"\n"
               "                    Without these, it plays on the device used last time, or the\n"
               "                    system default. --device alone picks the first driver listing it.\n"
               "  --watch           when a clip's .notes file is saved, play the change: it's sent\n"
               "                    as a write_clip, so it can be undone like any other edit\n";
    }

    juce::String juceString (const std::string& utf8)
    {
        return juce::String::fromUTF8 (utf8.data(), static_cast<int> (utf8.size()));
    }

    int checkProject (const std::string& folder)
    {
        const auto result = hocket::project::load (hocket::pathFromUtf8 (folder));

        for (const auto& error : result.errors)
            std::cerr << hocket::project::formatDiagnostic (error) << "\n";

        if (! result.project)
            return 1;

        const auto& project = *result.project;
        std::size_t clipCount = 0;

        for (const auto& track : project.tracks)
            clipCount += track.clips.size();

        std::cout << "OK: " << project.tempo << " BPM, " << project.meter.beatsPerBar << "/4, "
                  << project.tracks.size() << (project.tracks.size() == 1 ? " track, " : " tracks, ")
                  << clipCount << (clipCount == 1 ? " clip\n" : " clips\n");
        return 0;
    }

    void printDeviceNames (const juce::StringArray& names, std::string_view label)
    {
        std::cout << "  " << label << ":\n";

        if (names.isEmpty())
            std::cout << "    (none)\n";

        for (const auto& name : names)
            std::cout << "    " << name.toStdString() << "\n";
    }

    int listDevices()
    {
        tracktion::Engine engine { "Hocket", nullptr, std::make_unique<ListDevicesBehaviour>() };
        auto& deviceManager = engine.getDeviceManager().deviceManager;

        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            std::cout << type->getTypeName().toStdString() << "\n";
            printDeviceNames (type->getDeviceNames (false), "outputs");
            printDeviceNames (type->getDeviceNames (true), "inputs");
        }

        return 0;
    }

    //==============================================================================
    struct PlayOptions
    {
        std::string folder;
        std::string driver; // empty: the last one used, or the system default
        std::string device;
        bool watch = false;
    };

    std::optional<PlayOptions> parsePlayOptions (const std::vector<std::string>& arguments)
    {
        if (arguments.size() < 3 || arguments[1] != "--play")
            return {};

        PlayOptions options { arguments[2] };

        for (std::size_t i = 3; i < arguments.size(); ++i)
        {
            const auto hasValue = i + 1 < arguments.size();

            if (arguments[i] == "--watch")
                options.watch = true;
            else if (arguments[i] == "--driver" && hasValue)
                options.driver = arguments[++i];
            else if (arguments[i] == "--device" && hasValue)
                options.device = arguments[++i];
            else
                return {};
        }

        return options;
    }

    std::string quotedList (const juce::StringArray& names)
    {
        std::string list;

        for (const auto& name : names)
            list += (list.empty() ? "'" : ", '") + name.toStdString() + "'";

        return list.empty() ? "(none)" : list;
    }

    // Switches to the driver and device asked for. With neither, it keeps what
    // Tracktion opened: the device used last time (from its settings file), or
    // the system default. Returns an error, or an empty string.
    std::string openDevice (juce::AudioDeviceManager& devices, const PlayOptions& options)
    {
        if (options.driver.empty() && options.device.empty())
            return {};

        juce::AudioIODeviceType* type = nullptr;
        juce::StringArray typeNames;

        for (auto* candidate : devices.getAvailableDeviceTypes())
        {
            candidate->scanForDevices();
            typeNames.add (candidate->getTypeName());

            const auto matches = options.driver.empty() ? candidate->getDeviceNames (false).contains (juceString (options.device))
                                                        : candidate->getTypeName() == juceString (options.driver);
            if (matches && type == nullptr)
                type = candidate;
        }

        if (type == nullptr)
            return options.driver.empty() ? "no driver has an output device called '" + options.device + "'; --list-devices lists them"
                                          : "there's no driver called '" + options.driver + "'; the drivers are " + quotedList (typeNames);

        const auto deviceNames = type->getDeviceNames (false);
        const auto deviceName = options.device.empty() ? deviceNames[type->getDefaultDeviceIndex (false)] : juceString (options.device);

        if (! deviceNames.contains (deviceName))
            return type->getTypeName().toStdString() + " has no output device called '" + options.device + "'; its output devices are "
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

    // Runs on the message thread while --play plays: stops the message loop on
    // Ctrl+C and, with --watch, sends each saved clip file to the bus.
    class PlayLoop final : private juce::Timer
    {
    public:
        PlayLoop (hocket::commands::CommandBus& commandBus, const std::filesystem::path& folder, bool watch) : bus (commandBus)
        {
            if (watch)
                watcher.emplace (folder, bus.project());

            startTimer (50);
        }

        ~PlayLoop() override { stopTimer(); }

    private:
        hocket::commands::CommandBus& bus;
        std::optional<hocket::project::ClipFileWatcher> watcher;

        void timerCallback() override
        {
            if (hocket::interruptRequested())
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();
                return;
            }

            if (! watcher)
                return;

            for (const auto& change : watcher->poll())
            {
                if (! change.text)
                {
                    std::cerr << change.file << ": the file is gone or can't be read; clip " << change.clipId << " keeps its notes\n";
                    continue;
                }

                const auto result = bus.execute (hocket::commands::WriteClip { change.clipId, *change.text }, localUser);
                (result.ok ? std::cout : std::cerr) << change.file << ": " << result.message << std::endl;
            }
        }
    };

    int playProject (const PlayOptions& options)
    {
        const auto folder = hocket::pathFromUtf8 (options.folder);
        auto loaded = hocket::project::load (folder);

        for (const auto& error : loaded.errors)
            std::cerr << hocket::project::formatDiagnostic (error) << "\n";

        if (! loaded.project)
            return 1;

        // Opens the device used last time, or the system default. Tracktion
        // keeps its settings in %APPDATA%\Hocket\Settings.xml.
        tracktion::Engine engine { "Hocket", nullptr, std::make_unique<PlayBehaviour>() };
        auto& devices = engine.getDeviceManager().deviceManager;

        if (const auto error = openDevice (devices, options); ! error.empty())
        {
            std::cerr << error << "\n";
            return 1;
        }

        auto* device = devices.getCurrentAudioDevice();

        if (device == nullptr)
        {
            std::cerr << "no audio device could be opened; pick one with --driver and --device (--list-devices lists them)\n";
            return 1;
        }

        // Let Tracktion catch up with a device change before the Edit picks its outputs.
        devices.dispatchPendingMessages();
        engine.getDeviceManager().dispatchPendingUpdates();

        // Tracktion applies its first MIDI device scan on its 4-second rescan
        // timer (DeviceManager::initialise replaces the 5 ms one it starts), and
        // applying it rebuilds every playback context, which stops playback.
        // So get the scan done now, before there's anything to stop.
        engine.getDeviceManager().rescanMidiDeviceList();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

        const auto sampleRate = device->getCurrentSampleRate();
        const auto bufferSize = device->getCurrentBufferSizeSamples();
        std::cout << "Playing on " << device->getTypeName().toStdString() << ": " << device->getName().toStdString() << ", "
                  << sampleRate << " Hz, " << bufferSize << " samples ("
                  << juce::String (1000.0 * bufferSize / sampleRate, 1).toStdString() << " ms)\n";

        hocket::commands::CommandBus bus (std::move (*loaded.project));
        hocket::engine::EditSync sync (engine, bus);

        const auto played = bus.execute (hocket::commands::Play {}, localUser);

        if (! played.ok)
        {
            std::cerr << played.message << "\n";
            return 1;
        }

        std::cout << played.message << (options.watch ? "; saving a clip file plays the change" : "") << ". Ctrl+C to stop." << std::endl;

        hocket::catchInterrupts();
        {
            PlayLoop loop (bus, folder, options.watch);
            juce::MessageManager::getInstance()->runDispatchLoop();
        }

        std::cout << bus.execute (hocket::commands::Stop {}, localUser).message;

        if (const auto xruns = device->getXRunCount(); xruns >= 0)
            std::cout << "; the driver reported " << xruns << (xruns == 1 ? " xrun" : " xruns");

        std::cout << "\n";
        return 0;
    }

    int printVersion()
    {
        std::cout << "hocket-headless " << HOCKET_VERSION << "\n"
                  << "Tracktion Engine " << HOCKET_TRACKTION_VERSION << "\n"
                  << juce::SystemStats::getJUCEVersion().toStdString() << "\n";
        return 0;
    }
}

int main (int argc, char* argv[])
{
    hocket::initialiseConsole();
    const auto arguments = hocket::utf8Arguments (argc, argv);

    // Tracktion needs JUCE's message manager, even without a GUI.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (arguments.size() == 2)
    {
        if (arguments[1] == "--list-devices")
            return listDevices();

        if (arguments[1] == "--version")
            return printVersion();
    }

    if (arguments.size() == 3 && arguments[1] == "--check")
        return checkProject (arguments[2]);

    if (const auto options = parsePlayOptions (arguments))
        return playProject (*options);

    printUsage (std::cerr);
    return 1;
}
