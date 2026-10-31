// hocket-headless: the Milestone 0 command-line host (SPEC §10.0).
//
// It can check a project folder, list audio devices, and play a project on a
// loop. While it plays, it serves the WebSocket API that MCP clients reach
// through hocket-mcp (SPEC §6.3).

#include "Console.h"

#include "api/Handler.h"
#include "api/Protocol.h"
#include "api/Server.h"
#include "api/TokenFile.h"
#include "core/Paths.h"
#include "core/commands/CommandBus.h"
#include "core/project/ClipFileWatcher.h"
#include "core/project/Project.h"
#include "engine/EditSync.h"
#include "platform/Platform.h"

#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <iostream>
#include <optional>
#include <string_view>

namespace
{
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
               "       hocket-headless --play <project-folder> [--driver <type>] [--device <name>] [--watch] [--user <name>]\n"
               "                                                  play the project on a loop until Ctrl+C\n"
               "       hocket-headless --list-devices             list audio devices by driver type\n"
               "       hocket-headless --version                  print version numbers\n"
               "\n"
               "While --play plays, AI clients can work on the project through its API on 127.0.0.1;\n"
               "the port and token are in %LOCALAPPDATA%\\Hocket\\api.json until it stops.\n"
               "\n"
               "--play options:\n"
               "  --driver <type>   a driver type as --list-devices prints it, e.g. ASIO\n"
               "  --device <name>   an output device as --list-devices prints it, e.g. \"ASIO4ALL v2\"\n"
               "                    Without these, it plays on the device used last time, or the\n"
               "                    system default. --device alone picks the first driver listing it.\n"
               "  --watch           when a clip's .notes file is saved, play the change: it's sent\n"
               "                    as a write_clip, so it can be undone like any other edit\n"
               "  --user <name>     your name in the history and in messages to AI clients, e.g. \"was\n"
               "                    made by Lee, not Claude\". Default: user\n";
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
        std::string user = "user"; // the display name of the person at the DAW (SPEC §3.1)
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
            else if (arguments[i] == "--user" && hasValue && ! arguments[i + 1].empty())
                options.user = arguments[++i];
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

    // Deletes the token file when --play ends, unless another instance has replaced it since.
    struct TokenFileGuard
    {
        std::filesystem::path file;
        hocket::api::ApiInfo info;

        ~TokenFileGuard() { hocket::api::removeTokenFile (file, info); }
    };

    // Runs on the message thread while --play plays: stops the message loop on
    // Ctrl+C, closes API transactions left idle, and, with --watch, sends each
    // saved clip file to the bus.
    class PlayLoop final : private juce::Timer
    {
    public:
        PlayLoop (hocket::commands::CommandBus& commandBus, hocket::api::Handler& apiHandler, const std::filesystem::path& folder, bool watch,
                  hocket::commands::Author localUser)
            : bus (commandBus), api (apiHandler), user (std::move (localUser))
        {
            if (watch)
                watcher.emplace (folder, bus.project());

            startTimer (50);
        }

        ~PlayLoop() override { stopTimer(); }

    private:
        hocket::commands::CommandBus& bus;
        hocket::api::Handler& api;
        hocket::commands::Author user;
        std::optional<hocket::project::ClipFileWatcher> watcher;

        void timerCallback() override
        {
            if (hocket::interruptRequested())
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();
                return;
            }

            api.checkIdle();

            if (! watcher)
                return;

            for (const auto& change : watcher->poll())
            {
                if (! change.text)
                {
                    std::cerr << change.file << ": the file is gone or can't be read; clip " << change.clipId << " keeps its notes\n";
                    continue;
                }

                const auto result = bus.execute (hocket::commands::WriteClip { change.clipId, *change.text }, user);
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

        // Clients find the API through the token file, so only one Hocket at a time can serve it.
        const auto tokenFile = hocket::api::defaultTokenFilePath();

        if (! tokenFile)
        {
            std::cerr << "can't find the folder for the API's token file (%LOCALAPPDATA% on Windows)\n";
            return 1;
        }

        if (const auto running = hocket::api::runningInstance (*tokenFile))
        {
            std::cerr << "another Hocket is already running (" << running->program << ", process " << running->processId
                      << ", API on port " << running->port << "); stop it first\n";
            return 1;
        }

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

        // The person at the DAW: one fixed identity, so no session ID (SPEC §3.1).
        const hocket::commands::Author localUser { options.user };

        hocket::commands::CommandBus bus (std::move (*loaded.project));
        hocket::engine::EditSync sync (engine, bus);

        // The API for MCP clients (SPEC §6.3), with a new token each run.
        const auto token = hocket::api::makeToken();

        if (token.empty())
        {
            std::cerr << "couldn't get random bytes from the OS for the API's token\n";
            return 1;
        }

        const auto log = [] (const std::string& line) { std::cout << "API: " << line << std::endl; };

        hocket::api::HandlerOptions apiOptions;
        apiOptions.token = token;
        apiOptions.serverName = "hocket-headless";
        apiOptions.log = log;
        hocket::api::Handler api (bus, std::move (apiOptions));

        hocket::api::Server server (api, [] (std::function<void()> function) { juce::MessageManager::callAsync (std::move (function)); },
                                    { .log = log });

        if (const auto error = server.start(); ! error.empty())
        {
            std::cerr << error << "\n";
            return 1;
        }

        const hocket::api::ApiInfo apiInfo { server.port(), token, hocket::platform::currentProcessId(), hocket::platform::currentProgramName() };

        if (const auto error = hocket::api::writeTokenFile (*tokenFile, apiInfo); ! error.empty())
        {
            std::cerr << "couldn't write the API's token file: " << error << "\n";
            return 1;
        }

        const TokenFileGuard tokenFileGuard { *tokenFile, apiInfo };
        std::cout << "API: ws://127.0.0.1:" << server.port() << " (token in " << hocket::utf8FromPath (*tokenFile) << ")\n";

        const auto played = bus.execute (hocket::commands::Play {}, localUser);

        if (! played.ok)
        {
            std::cerr << played.message << "\n";
            return 1;
        }

        std::cout << played.message << (options.watch ? "; saving a clip file plays the change" : "") << ". Ctrl+C to stop." << std::endl;

        hocket::catchInterrupts();
        {
            PlayLoop loop (bus, api, folder, options.watch, localUser);
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
        std::cout << "hocket-headless " << HOCKET_VERSION << " (API build " << hocket::api::buildFingerprint() << ")\n"
                  << "Tracktion Engine " << HOCKET_TRACKTION_VERSION << "\n"
                  << juce::SystemStats::getJUCEVersion().toStdString() << "\n";
        return 0;
    }
}

int main (int argc, char* argv[])
{
    hocket::initialiseConsole();
    const auto arguments = hocket::platform::utf8Arguments (argc, argv);

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
