// hocket-headless: the Milestone 0 command-line host (SPEC §10.0).
//
// So far it can list audio devices and check a project folder. Playing a
// project, and the API for MCP clients, come in later steps.

#include "Console.h"

#include "core/Paths.h"
#include "core/project/Project.h"

#include <tracktion_engine/tracktion_engine.h>

#include <iostream>
#include <string_view>

namespace
{
    // Listing devices must not open one, so Tracktion's device manager isn't
    // initialised on startup.
    struct ListDevicesBehaviour final : tracktion::EngineBehaviour
    {
        bool autoInitialiseDeviceManager() override { return false; }
    };

    void printUsage (std::ostream& out)
    {
        out << "usage: hocket-headless --check <project-folder>   validate a project; errors go to stderr\n"
               "       hocket-headless --list-devices             list audio devices by driver type\n"
               "       hocket-headless --version                  print version numbers\n";
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

    printUsage (std::cerr);
    return 1;
}
