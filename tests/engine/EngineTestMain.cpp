// The engine tests' main: JUCE's message manager and one Tracktion Engine
// exist for the whole run.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "EngineTesting.h"
#include "common/TemporaryFolder.h"

#include <cassert>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <timeapi.h>
#endif

namespace hocket::testing
{
namespace
{
    tracktion::Engine* sharedEngine = nullptr;
    juce::File folder;

    struct TestPropertyStorage final : tracktion::PropertyStorage
    {
        TestPropertyStorage() : PropertyStorage ("Hocket tests") {}

        juce::File getAppPrefsFolder() override { return folder; }
        juce::File getAppCacheFolder() override { return folder; }
    };

    struct TestBehaviour final : tracktion::EngineBehaviour
    {
        bool autoInitialiseDeviceManager() override { return false; }
    };

    // Tracktion's play() waits for the audio device to play a block, polling
    // with Thread::sleep (1) up to 200 times (DeviceManager::addContext). The
    // tests play blocks themselves, after play(), so it always polls all 200
    // times, and that takes as long as 200 of the system timer's ticks: 0.3 s
    // with 1 ms timers, 2 to 3 s with the default 10 to 15.6 ms. Windows 11
    // stops honouring a process's 1 ms timers when it isn't playing audio it
    // can hear, which the fake device isn't, so play() went from 0.55 s to
    // 2.5 s part-way through a run, on some days and not others (PROGRESS.md,
    // Loose ends). So the tests ask for 1 ms timers, and to keep them. A real
    // device plays blocks all the time, so --play's poll ends within a block.
    struct FineTimers
    {
        FineTimers()
        {
           #ifdef _WIN32
            PROCESS_POWER_THROTTLING_STATE throttling {};
            throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
            throttling.StateMask = 0; // not ignored: honoured
            SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof (throttling));
            timeBeginPeriod (1);
           #endif
        }

        ~FineTimers()
        {
           #ifdef _WIN32
            timeEndPeriod (1);
           #endif
        }
    };
}

tracktion::Engine& testEngine()
{
    assert (sharedEngine != nullptr);
    return *sharedEngine;
}

juce::File testFolder()
{
    return folder;
}
}

int main (int argc, char** argv)
{
    using namespace hocket::testing;

    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const FineTimers fineTimers;

    // Non-ASCII, so renders and Tracktion's own files are tested with such a path too.
    folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getNonexistentChildFile (juce::String::fromUTF8 (("hocket-engine-tests " + unicodeName).c_str()), {});
    folder.createDirectory();

    int result = 0;
    {
        tracktion::Engine tracktionEngine { std::make_unique<TestPropertyStorage>(), nullptr, std::make_unique<TestBehaviour>() };
        sharedEngine = &tracktionEngine;
        result = doctest::Context (argc, argv).run();
        sharedEngine = nullptr;
    }

    folder.deleteRecursively();
    return result;
}
