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
#include "core/kits/KitCatalog.h"
#include "core/project/Project.h"
#include "core/units/Units.h"
#include "engine/EditSync.h"
#include "engine/MasterOutput.h"
#include "engine/PlaybackDevice.h"
#include "engine/kits/KitLibrary.h"
#include "platform/Platform.h"
#include "storage/ProjectFolder.h"
#include "storage/Recovery.h"
#include "storage/SaveKeys.h"
#include "storage/WatchedFolder.h"

#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
               "       hocket-headless --new <project-folder> [--user <name>]\n"
               "                                                  make an empty project (120 BPM, 4/4, no tracks)\n"
               "                                                  in a new or empty folder\n"
               "       hocket-headless --play <project-folder> [--driver <type>] [--device <name>]\n"
               "                       [--sample-rate <Hz>] [--buffer-size <samples>] [--master-volume <dB>]\n"
               "                       [--watch] [--user <name>] [--recover | --discard-recovery]\n"
               "                                                  play the project on a loop until Ctrl+C\n"
               "       hocket-headless --list-devices [--driver <type>] [--device <name>]\n"
               "                                                  list audio devices by driver type; for a driver\n"
               "                                                  or device named, also the sample rates and\n"
               "                                                  buffer sizes it offers\n"
               "       hocket-headless --version                  print version numbers\n"
               "\n"
               "While --play plays, AI clients can work on the project through its API on 127.0.0.1;\n"
               "the port and token are in %LOCALAPPDATA%\\Hocket\\api.json until it stops.\n"
               "\n"
               "--play options:\n"
               "  --driver <type>           a driver type as --list-devices prints it, e.g. ASIO\n"
               "  --device <name>           an output device as --list-devices prints it, e.g. \"ASIO4ALL v2\"\n"
               "                            Without these, it plays on the device used last time, or the\n"
               "                            system default. --device alone picks the first driver listing it.\n"
               "  --sample-rate <Hz>        e.g. 48000, and\n"
               "  --buffer-size <samples>   e.g. 256: values the device offers (--list-devices --device <name>\n"
               "                            lists them). Without these, it keeps the device's last ones.\n"
               "                            The device, sample rate and buffer size are kept for the next run.\n"
               "  --master-volume <dB>      the level of everything it plays, written like a track's volume:\n"
               "                            e.g. -6dB, -infdB (silence). Default 0dB, as the project mixes it.\n"
               "                            Some drivers bypass the Windows volume slider: turn the device down first.\n"
               "                            Not kept. At Ctrl+C it prints the peak output level.\n"
               "  --watch                   when project.json or a clip's .notes file is saved outside Hocket,\n"
               "                            play the change: it's sent as your command (a write_clip, or one\n"
               "                            replacing the project), so it can be undone like any other edit\n"
               "  --user <name>             your name in the history and in messages to AI clients, e.g. \"was\n"
               "                            made by Lee, not Claude\". Default: user\n"
               "  --recover                 play the recovery copy an earlier session left, with changes it\n"
               "                            never saved: they come in as one history entry, unsaved, which\n"
               "                            undo takes back. Without this or --discard-recovery, --play won't\n"
               "                            play a project that has one\n"
               "  --discard-recovery        play the files as saved, filing that recovery copy with the project's\n"
               "                            kept saves, so revert can still bring it back\n"
               "\n"
               "\n"
               "Keys while --play plays:\n"
               "  S                         save the project, as --user. A save never overwrites files changed\n"
               "                            outside Hocket: it says which, and offers O\n"
               "  O                         straight after S was refused for files changed outside Hocket:\n"
               "                            save over them, keeping what's on disk as a kept save first\n"
               "\n"
               "A few seconds after changes stop, --play writes the project to a recovery copy in\n"
               "%LOCALAPPDATA%\\Hocket\\Recovery, so a crash loses nothing. It's removed once nothing is unsaved.\n";
    }

    // A whole number above zero, or nullopt.
    std::optional<int> positiveNumber (const std::string& text)
    {
        int value = 0;
        const auto* end = text.data() + text.size();
        const auto [next, error] = std::from_chars (text.data(), end, value);

        if (error != std::errc() || next != end || value <= 0)
            return {};

        return value;
    }

    // The kits Hocket can play (SPEC §8.4): the bundled ones, in kits\ next to
    // this program, and the user's own, in Documents\Hocket\Kits.
    hocket::kits::KitCatalog installedKits()
    {
        const auto documents = hocket::platform::documentsFolder();
        return { hocket::platform::currentProgramFolder() / hocket::pathFromUtf8 ("kits"),
                 documents ? *documents / hocket::pathFromUtf8 ("Hocket") / hocket::pathFromUtf8 ("Kits") : std::filesystem::path() };
    }

    // A note for each drums track whose kit can't play, saying why: the
    // project is still valid (SPEC §8.4), but the track is silent.
    void reportKits (const hocket::project::Project& project, hocket::kits::KitCatalog& kits)
    {
        for (const auto& track : project.tracks)
            for (const auto& device : track.devices)
                if (device.type == hocket::project::DeviceType::drumSampler)
                    if (const auto found = kits.find (device.kit); found.kit == nullptr)
                        std::cerr << "Note: track " << track.id << " is silent: " << found.problem << ".\n";
    }

    // The installed kits the project's drum samplers play, for --play to say
    // it's loading them: "kit 'electronic'", "kits 'electronic' and
    // 'user/dusty-909'"; empty if there are none.
    std::string kitsInUse (const hocket::project::Project& project, hocket::kits::KitCatalog& kits)
    {
        std::vector<std::string> names;

        for (const auto& track : project.tracks)
            for (const auto& device : track.devices)
                if (device.type == hocket::project::DeviceType::drumSampler && kits.find (device.kit).kit != nullptr)
                    if (const auto quoted = "'" + device.kit + "'"; std::ranges::find (names, quoted) == names.end())
                        names.push_back (quoted);

        if (names.empty())
            return {};

        std::string text = names.size() == 1 ? "kit " : "kits ";

        for (std::size_t i = 0; i < names.size(); ++i)
            text += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];

        return text;
    }

    // What loading the kits in use found that their headers couldn't say:
    // samples the decoder refused, and pieces that peak above full scale. The
    // overview says the same.
    void reportKitLoads (const hocket::project::Project& project, hocket::kits::KitCatalog& kits)
    {
        std::vector<std::string> reported;

        for (const auto& track : project.tracks)
            for (const auto& device : track.devices)
            {
                if (device.type != hocket::project::DeviceType::drumSampler || std::ranges::find (reported, device.kit) != reported.end())
                    continue;

                reported.push_back (device.kit);
                const auto* kit = kits.find (device.kit).kit;

                if (kit == nullptr)
                    continue;

                for (const auto& problem : kit->loaded.problems)
                    std::cerr << "Note: kit '" << device.kit << "': " << problem << ".\n";

                for (const auto& loud : kit->loaded.overFullScale)
                {
                    char peak[32];
                    std::snprintf (peak, sizeof (peak), "%+.2f", loud.peak);
                    std::cerr << "Note: kit '" << device.kit << "': piece " << loud.piece << "'s sample peaks at " << peak << " dBFS, above full scale.\n";
                }
            }
    }

    int checkProject (const std::string& folder)
    {
        const auto result = hocket::project::load (hocket::pathFromUtf8 (folder));

        for (const auto& error : result.errors)
            std::cerr << hocket::project::formatDiagnostic (error) << "\n";

        // It changes nothing, so it only says so; --play finishes it, or
        // says why it can't (SPEC §4.5).
        if (const auto note = hocket::storage::describeInterruptedSave (hocket::pathFromUtf8 (folder)); ! note.empty())
            std::cerr << note << "\n";

        if (! result.project)
            return 1;

        const auto& project = *result.project;
        std::size_t clipCount = 0;

        for (const auto& track : project.tracks)
            clipCount += track.clips.size();

        std::cout << "OK: " << project.tempo << " BPM, " << project.meter.text() << ", "
                  << project.tracks.size() << (project.tracks.size() == 1 ? " track, " : " tracks, ")
                  << clipCount << (clipCount == 1 ? " clip" : " clips");

        if (const auto files = project.notesFiles.size(); files < clipCount)
            std::cout << " in " << files << (files == 1 ? " file" : " files") << ", as linked clips share them";

        std::cout << "\n";

        auto kits = installedKits();
        reportKits (project, kits);
        return 0;
    }

    struct NewOptions
    {
        std::string folder;
        std::string user = "user";
    };

    std::optional<NewOptions> parseNewOptions (const std::vector<std::string>& arguments)
    {
        if (arguments.size() < 3 || arguments[1] != "--new")
            return {};

        NewOptions options { arguments[2] };

        for (std::size_t i = 3; i < arguments.size(); ++i)
        {
            if (arguments[i] == "--user" && i + 1 < arguments.size() && ! arguments[i + 1].empty())
                options.user = arguments[++i];
            else
                return {};
        }

        return options;
    }

    int newProject (const NewOptions& options)
    {
        const auto folder = hocket::pathFromUtf8 (options.folder);

        if (const auto error = hocket::storage::createProject (folder, { options.user }); ! error.empty())
        {
            std::cerr << error << "\n";
            return 1;
        }

        std::error_code error;
        const auto absolute = std::filesystem::absolute (folder, error);
        std::cout << "Made an empty project in " << hocket::utf8FromPath (error ? folder : absolute)
                  << ": 120 BPM, 4/4, no tracks. --play plays it, and serves the API to build it with.\n";
        return 0;
    }

    struct ListOptions
    {
        std::string driver; // empty: every driver
        std::string device; // empty: every device
    };

    std::optional<ListOptions> parseListOptions (const std::vector<std::string>& arguments)
    {
        if (arguments.size() < 2 || arguments[1] != "--list-devices")
            return {};

        ListOptions options;

        for (std::size_t i = 2; i < arguments.size(); ++i)
        {
            const auto hasValue = i + 1 < arguments.size();

            if (arguments[i] == "--driver" && hasValue)
                options.driver = arguments[++i];
            else if (arguments[i] == "--device" && hasValue)
                options.device = arguments[++i];
            else
                return {};
        }

        return options;
    }

    int listDevices (const ListOptions& options)
    {
        tracktion::Engine engine { "Hocket", nullptr, std::make_unique<ListDevicesBehaviour>() };
        const auto list = hocket::engine::listDevices (engine.getDeviceManager().deviceManager, options.driver, options.device);

        if (! list.error.empty())
        {
            std::cerr << list.error << "\n";
            return 1;
        }

        std::cout << list.text;
        return 0;
    }

    //==============================================================================
    struct PlayOptions
    {
        std::string folder;
        hocket::engine::DeviceChoice device; // empty and zero: as last time, or the system default
        double masterVolume = hocket::engine::defaultMasterVolume; // dB (SPEC §9)
        bool watch = false;
        std::string user = "user"; // the display name of the person at the DAW (SPEC §3.1)
        hocket::storage::RecoveryChoice recovery = hocket::storage::RecoveryChoice::none;
        std::string error;         // an option's value that's wrong, and why
    };

    std::optional<PlayOptions> parsePlayOptions (const std::vector<std::string>& arguments)
    {
        if (arguments.size() < 3 || arguments[1] != "--play")
            return {};

        PlayOptions options { arguments[2] };

        for (std::size_t i = 3; i < arguments.size(); ++i)
        {
            const auto hasValue = i + 1 < arguments.size();
            const auto number = hasValue ? positiveNumber (arguments[i + 1]) : std::nullopt;

            if (arguments[i] == "--watch")
                options.watch = true;
            else if (arguments[i] == "--recover" || arguments[i] == "--discard-recovery")
            {
                const auto choice = arguments[i] == "--recover" ? hocket::storage::RecoveryChoice::recover : hocket::storage::RecoveryChoice::discard;

                if (options.recovery != hocket::storage::RecoveryChoice::none && options.recovery != choice && options.error.empty())
                    options.error = "--recover and --discard-recovery don't go together: --recover plays the recovery copy, --discard-recovery "
                                    "files it with the kept saves and plays the files as saved. Pick one";

                options.recovery = choice;
            }
            else if (arguments[i] == "--driver" && hasValue)
                options.device.driver = arguments[++i];
            else if (arguments[i] == "--device" && hasValue)
                options.device.device = arguments[++i];
            else if (arguments[i] == "--sample-rate" && number)
            {
                options.device.sampleRate = *number;
                ++i;
            }
            else if (arguments[i] == "--buffer-size" && number)
            {
                options.device.bufferSize = *number;
                ++i;
            }
            else if (arguments[i] == "--user" && hasValue && ! arguments[i + 1].empty())
                options.user = arguments[++i];
            else if (arguments[i] == "--master-volume" && hasValue)
            {
                const auto parsed = hocket::units::parse (arguments[++i], hocket::units::volume());

                if (parsed.value)
                    options.masterVolume = *parsed.value;
                else if (options.error.empty())
                    options.error = "--master-volume: " + parsed.error;
            }
            else
                return {};
        }

        return options;
    }

    // When --play ends, however it ends once the bus has a project: removes the
    // recovery copy if nothing is unsaved, or makes sure it's up to date and
    // says where it is (SPEC §4.5).
    struct RecoveryFinish
    {
        hocket::storage::RecoveryKeeper& keeper;

        ~RecoveryFinish()
        {
            try
            {
                if (const auto said = keeper.finish(); ! said.empty())
                    std::cout << said << std::endl;
            }
            catch (const std::exception& error)
            {
                std::cerr << "internal error while finishing the recovery copy: " << error.what() << std::endl;
            }
        }
    };

    // Deletes the token file when --play ends, unless another instance has replaced it since.
    struct TokenFileGuard
    {
        std::filesystem::path file;
        hocket::api::ApiInfo info;

        ~TokenFileGuard() { hocket::api::removeTokenFile (file, info); }
    };

    // Runs on the message thread while --play plays: stops the message loop on
    // Ctrl+C, says when playback stops by itself, closes API transactions left
    // idle, keeps the recovery copy up to date, saves when S is pressed, and,
    // with --watch, sends each project file saved outside Hocket to the bus.
    class PlayLoop final : private juce::Timer
    {
    public:
        PlayLoop (hocket::commands::CommandBus& commandBus, const hocket::engine::EditSync& editSync,
                  const hocket::engine::PlaybackDevice& playbackDevice, hocket::api::Handler& apiHandler,
                  hocket::storage::ProjectFolder& projectFolder, hocket::storage::RecoveryKeeper& recoveryKeeper, bool watch,
                  hocket::commands::Author localUser)
            : sync (editSync), device (playbackDevice), api (apiHandler), recovery (recoveryKeeper), saveKeys (commandBus, localUser),
              deviceWhilePlaying (playbackDevice.description())
        {
            if (watch)
                watched.emplace (commandBus, projectFolder, std::move (localUser));

            startTimer (50);
        }

        ~PlayLoop() override { stopTimer(); }

    private:
        const hocket::engine::EditSync& sync;
        const hocket::engine::PlaybackDevice& device;
        hocket::api::Handler& api;
        hocket::storage::RecoveryKeeper& recovery;
        hocket::storage::SaveKeys saveKeys;
        std::optional<hocket::storage::WatchedFolder> watched;
        std::string deviceWhilePlaying; // the device's description, as of the last check
        bool stopReported = false;

        void timerCallback() override
        {
            if (hocket::interruptRequested())
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();
                return;
            }

            // An exception here would end --play, music and all, so it's reported instead.
            try
            {
                reportStopByItself();
                api.checkIdle();
                pollWatcher();

                for (const auto key : hocket::typedKeys())
                    if (const auto said = saveKeys.press (key))
                        std::cout << *said << std::endl;

                if (const auto said = recovery.poll())
                    std::cerr << *said << std::endl;
            }
            catch (const std::exception& error)
            {
                std::cerr << "internal error: " << error.what() << std::endl;
            }
        }

        // Once per stop, so that it isn't taken for silence or a crash. A
        // change of device is the likely cause, so it says what changed.
        void reportStopByItself()
        {
            if (! sync.stoppedByItself())
            {
                stopReported = false;
                deviceWhilePlaying = device.description();
                return;
            }

            if (stopReported)
                return;

            stopReported = true;
            const auto now = device.description();

            const auto why = now.empty()                ? std::string ("the audio device closed")
                           : now != deviceWhilePlaying ? "the audio device changed, and is now " + now
                                                       : std::string ("most likely Tracktion rebuilt its device lists, as it does when a MIDI or "
                                                                      "audio device is plugged in, unplugged, or reset by its driver");

            std::cerr << "Playback stopped by itself: " << why << ". An API client's transport play starts it again; Ctrl+C quits."
                      << std::endl;
        }

        void pollWatcher()
        {
            if (! watched)
                return;

            for (const auto& report : watched->poll())
            {
                (report.ok ? std::cout : std::cerr) << report.file << ": " << report.message << std::endl;

                if (report.warning)
                    std::cerr << report.file << ": " << *report.warning << std::endl;
            }
        }
    };

    int playProject (const PlayOptions& options)
    {
        if (! options.error.empty())
        {
            std::cerr << options.error << "\n";
            return 1;
        }

        const auto folder = hocket::pathFromUtf8 (options.folder);

        // Clients find the API through the token file, so only one Hocket at a time can serve it.
        const auto tokenFile = hocket::api::defaultTokenFilePath();

        if (! tokenFile)
        {
            std::cerr << "can't find the folder for the API's token file (%LOCALAPPDATA% on Windows)\n";
            return 1;
        }

        const auto reportRunning = [&tokenFile]
        {
            if (const auto running = hocket::api::runningInstance (*tokenFile))
            {
                std::cerr << "another Hocket is already running (" << running->program << ", process " << running->processId
                          << ", API on port " << running->port << "); stop it first\n";
                return true;
            }

            return false;
        };

        // Held until --play ends, so another --play starting now can't also
        // find none running. Taken first, before anything on disk changes:
        // no other Hocket can then be saving into the folder, or writing its
        // recovery copy.
        std::error_code folderError;
        std::filesystem::create_directories (tokenFile->parent_path(), folderError);
        const auto lockFile = hocket::api::instanceLockFile (*tokenFile);
        const auto instanceLock = hocket::platform::FileLock::acquire (lockFile);

        if (! instanceLock)
        {
            if (! reportRunning())
                std::cerr << "another Hocket is starting (it holds " << hocket::utf8FromPath (lockFile) << "); stop it first\n";

            return 1;
        }

        // A Hocket without the lock (an older build) still writes the token file.
        if (reportRunning())
            return 1;

        // A save a crash interrupted part-way (SPEC §4.5): finished as it
        // meant, all of it, before the folder is read. One that can't be, or
        // whose finishing fails part-way, stops --play: a session started on
        // the folder would be playing, and saving over, half of it.
        const auto finished = hocket::storage::finishInterruptedSave (folder);

        if (! finished.message.empty())
            (finished.proceed() ? std::cout : std::cerr) << finished.message << std::endl;

        if (! finished.proceed())
            return 1;

        // Where saves go (SPEC §4.5), noting each file's stamp as it's read,
        // so a save never overwrites a change made outside Hocket unasked.
        // Made before the bus, so it outlives it.
        auto [loaded, projectFolder] = hocket::storage::ProjectFolder::open (folder);

        for (const auto& error : loaded.errors)
            std::cerr << hocket::project::formatDiagnostic (error) << "\n";

        // A recovery copy an earlier session left (SPEC §4.5): looked for once
        // no other Hocket can be writing it. Plain --play stops if it holds
        // unsaved changes, before opening the device.
        const auto recoveryFolder = hocket::storage::defaultRecoveryFolder();

        if (! recoveryFolder)
        {
            std::cerr << "can't find the folder for recovery copies (%LOCALAPPDATA% on Windows)\n";
            return 1;
        }

        hocket::storage::RecoveryCopy recoveryCopy (*recoveryFolder, folder);
        hocket::storage::RecoveryStart recoveryStart;
        std::optional<hocket::project::Project> startProject;
        auto startsUnsaved = false;

        if (loaded.project)
        {
            recoveryStart = hocket::storage::startRecovery (recoveryCopy, *projectFolder, *loaded.project, options.recovery);
            startProject = std::move (loaded.project);
        }
        else
        {
            // Files that don't load: what there is to go back to, and with
            // --recover, the recovery copy in their place, unsaved.
            auto unloadable = hocket::storage::startUnloadable (recoveryCopy, folder, options.recovery);
            recoveryStart = { unloadable.proceed, std::move (unloadable.message) };
            startProject = std::move (unloadable.recovered);
            projectFolder = std::move (unloadable.folder);
            startsUnsaved = true;
        }

        if (! recoveryStart.message.empty())
            (recoveryStart.proceed ? std::cout : std::cerr) << recoveryStart.message << std::endl;

        if (! recoveryStart.proceed)
            return 1;

        // Kit folders Hocket can't use, then tracks whose kit can't play.
        auto kits = installedKits();

        for (const auto& problem : kits.problems())
            std::cerr << "Note: " << problem << "\n";

        reportKits (*startProject, kits);

        // From here on, Ctrl+C (or Ctrl+Break, or closing the window) stops
        // --play cleanly. Before play starts, that's at the next place it can
        // stop: once the device has opened, or while the kits load, closing
        // the device on the way out (step 8).
        hocket::catchInterrupts();

        const auto stoppedBeforePlaying = [] (std::string_view when)
        {
            if (! hocket::interruptRequested())
                return false;

            std::cout << "stopped before playing" << when << std::endl;
            return true;
        };

        // Opens the device used last time, or the system default. Tracktion
        // keeps its settings in %APPDATA%\Hocket\Settings.xml.
        tracktion::Engine engine { "Hocket", nullptr, std::make_unique<PlayBehaviour>() };
        auto& devices = engine.getDeviceManager().deviceManager;

        // Without --driver, --device, --sample-rate and --buffer-size, it keeps what Tracktion opened.
        if (const auto error = hocket::engine::openDevice (devices, options.device); ! error.empty())
        {
            std::cerr << error << "\n";
            return 1;
        }

        if (devices.getCurrentAudioDevice() == nullptr)
        {
            std::cerr << hocket::engine::noDeviceOpened (devices) << "\n";
            return 1;
        }

        // Let Tracktion catch up with a device change before the Edit picks its outputs.
        devices.dispatchPendingMessages();
        engine.getDeviceManager().dispatchPendingUpdates();

        // Tracktion applies its first MIDI device scan on its 4-second rescan
        // timer (DeviceManager::initialise replaces the 5 ms one it starts), and
        // applying it rebuilds every playback context, which stops playback.
        // So get the scan done now, before there's anything to stop. A Tracktion
        // bug: every Tracktion update checks whether this is still needed
        // (DEPENDENCIES.md, "Updating Tracktion").
        engine.getDeviceManager().rescanMidiDeviceList();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

        const hocket::engine::PlaybackDevice playbackDevice (devices);
        std::cout << "Playing on " << playbackDevice.description() << "\n";

        if (const auto warning = playbackDevice.volumeWarning(); ! warning.empty())
            std::cerr << warning << std::endl;

        // Before anything plays. Declared after the engine, so it's destroyed first.
        hocket::engine::MasterOutput master (engine.getDeviceManager(), options.masterVolume);
        std::cout << "Master level " << hocket::units::format (master.volume(), hocket::units::volume()) << " (--master-volume <dB> changes it)\n";

        if (stoppedBeforePlaying (""))
            return 0;

        // The person at the DAW: one fixed identity, so no session ID (SPEC §3.1).
        const hocket::commands::Author localUser { options.user };

        hocket::commands::CommandBus bus (std::move (*startProject));
        bus.setKitCatalog (&kits); // made before the bus, so it outlives it
        bus.setStorage (&*projectFolder);
        bus.setOutputMeter (&master); // made before the bus, so it outlives it

        // Started as the recovery copy, since the files don't load: not what's on disk.
        if (startsUnsaved)
            bus.markUnsaved();

        // --recover: the copy's changes, as one entry the person can undo,
        // unsaved, before the Edit is built, so it's built with them.
        if (recoveryStart.recovered)
        {
            const auto recovered = bus.execute (hocket::commands::ReplaceProject { std::move (*recoveryStart.recovered), recoveryStart.description }, localUser);

            if (! recovered.ok)
            {
                std::cerr << "--recover: " << recovered.message << "\n";
                return 1;
            }

            std::cout << "Recovered: " << recovered.message << std::endl;
        }

        hocket::storage::RecoveryKeeper recoveryKeeper (bus, *projectFolder, recoveryCopy, recoveryStart.recovered.has_value() || startsUnsaved);
        const RecoveryFinish recoveryFinish { recoveryKeeper };

        // The drum samplers' kits load on the library's thread, and play waits
        // for them. Made before the EditSync, whose samplers ask it for kits.
        // It says when a user kit in use changes on disk, and reloads it.
        hocket::engine::KitLibrary kitLibrary (kits);
        kitLibrary.setNotices ([] (const std::string& notice) { std::cout << notice << std::endl; });

        // Said before and after, so a Ctrl+C can be timed to land while they
        // load (step 8's check, tools/acceptance/Check-CtrlCWhileLoading.ps1).
        const auto loading = kitsInUse (bus.project(), kits);

        if (! loading.empty())
            std::cout << "Loading " << loading << std::endl;

        const auto loadStarted = std::chrono::steady_clock::now();
        hocket::engine::EditSync sync (engine, bus, &kitLibrary);

        while (! kitLibrary.waitUntilIdle (std::chrono::milliseconds (20)))
            if (stoppedBeforePlaying (", while the kits were loading"))
                return 0; // the library lets the kit it's building finish, then drops it

        if (! loading.empty())
            std::cout << "Loaded in " << std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - loadStarted).count()
                      << " ms" << std::endl;

        reportKitLoads (bus.project(), kits);

        if (stoppedBeforePlaying (""))
            return 0;

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

        std::cout << played.message << (options.watch ? "; saving project.json or a clip file outside Hocket plays the change" : "") << ". "
                  << hocket::storage::SaveKeys::hint << "; Ctrl+C to stop." << std::endl;

        {
            PlayLoop loop (bus, sync, playbackDevice, api, *projectFolder, recoveryKeeper, options.watch, localUser);
            juce::MessageManager::getInstance()->runDispatchLoop();
        }

        std::cout << bus.execute (hocket::commands::Stop {}, localUser).message << "; " << playbackDevice.xrunReport() << "; "
                  << hocket::engine::describePeak (master.peak()) << std::endl;

        if (const auto warning = hocket::engine::clippingWarning (master.peak(), master.clippedSamples(), master.volume()); ! warning.empty())
            std::cerr << warning << "\n";

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

    // Say what went wrong rather than end with no message (e.g. if Tracktion
    // can't create the Edit). Exceptions in the message loop's callbacks are
    // caught where they run (PlayLoop, the API's Handler and Server).
    try
    {
        if (arguments.size() == 2 && arguments[1] == "--version")
            return printVersion();

        if (const auto options = parseListOptions (arguments))
            return listDevices (*options);

        if (arguments.size() == 3 && arguments[1] == "--check")
            return checkProject (arguments[2]);

        if (const auto options = parseNewOptions (arguments))
            return newProject (*options);

        if (const auto options = parsePlayOptions (arguments))
            return playProject (*options);
    }
    catch (const std::exception& error)
    {
        std::cerr << "hocket-headless stopped on an internal error: " << error.what() << "\n";
        return 1;
    }

    printUsage (std::cerr);
    return 1;
}
