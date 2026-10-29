// hocket-headless: the Milestone 0 command-line host (SPEC §10.0).
//
// So far it can start Tracktion Engine and list audio devices. Loading and
// playing a project, and the API for MCP clients, come in later steps.

#include "Console.h"

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
        out << "usage: hocket-headless --list-devices   list audio devices by driver type\n"
               "       hocket-headless --version        print version numbers\n";
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

    // Tracktion needs JUCE's message manager, even without a GUI.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (argc == 2)
    {
        const std::string_view option { argv[1] };

        if (option == "--list-devices")
            return listDevices();

        if (option == "--version")
            return printVersion();
    }

    printUsage (std::cerr);
    return 1;
}
