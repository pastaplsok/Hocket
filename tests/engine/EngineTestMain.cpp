// The engine tests' main: JUCE's message manager and one Tracktion Engine
// exist for the whole run.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "EngineTesting.h"
#include "common/TemporaryFolder.h"

#include <cassert>

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
