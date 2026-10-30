#include "api/TokenFile.h"
#include "common/TemporaryFolder.h"
#include "platform/Platform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <fstream>

using namespace hocket;

namespace
{
    api::ApiInfo thisProcess (int port = 46253)
    {
        return { port, api::makeToken(), platform::currentProcessId(), platform::currentProgramName() };
    }

    bool isHex (const std::string& text)
    {
        return std::ranges::all_of (text, [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
}

TEST_CASE ("tokens are 64 hex digits and never repeat")
{
    const auto token = api::makeToken();
    CHECK (token.size() == 64);
    CHECK (isHex (token));
    CHECK (api::makeToken() != token);
}

TEST_CASE ("the OS's secure random source fills every byte")
{
    std::array<std::uint8_t, 64> a {}, b {};
    REQUIRE (platform::secureRandomBytes (a));
    REQUIRE (platform::secureRandomBytes (b));
    CHECK (a != b);
}

TEST_CASE ("this process is recognized as running, by ID and program name")
{
    CHECK (platform::currentProgramName().starts_with ("hocket_api_tests"));
    CHECK (platform::isProgramRunning (platform::currentProcessId(), platform::currentProgramName()));
    CHECK_FALSE (platform::isProgramRunning (platform::currentProcessId(), "hocket-headless.exe"));
    CHECK_FALSE (platform::isProgramRunning (0xfffffff0u, platform::currentProgramName()));
}

TEST_CASE ("the default token file is in the user's local app data folder")
{
    const auto file = api::defaultTokenFilePath();
    REQUIRE (file.has_value());
    CHECK (file->filename() == "api.json");
    CHECK (file->parent_path().filename() == "Hocket");
    CHECK (std::filesystem::is_directory (file->parent_path().parent_path()));
}

TEST_CASE ("the token file is written, read back and removed")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto file = folder.path / pathFromUtf8 ("new folder") / "api.json";
    const auto info = thisProcess();

    REQUIRE (api::writeTokenFile (file, info).empty());
    CHECK (api::readTokenFile (file) == info);
    CHECK (std::distance (std::filesystem::directory_iterator (file.parent_path()), {}) == 1); // no temporary file left

    // A newer instance replaces it, and the older one's removal leaves the newer file alone.
    const auto newer = thisProcess (46254);
    REQUIRE (api::writeTokenFile (file, newer).empty());
    api::removeTokenFile (file, info);
    CHECK (api::readTokenFile (file) == newer);

    api::removeTokenFile (file, newer);
    CHECK_FALSE (std::filesystem::exists (file));
}

TEST_CASE ("a running instance is spotted, and a file left by a crash isn't one")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto file = folder.path / "api.json";

    CHECK_FALSE (api::runningInstance (file).has_value()); // no file

    auto info = thisProcess();
    REQUIRE (api::writeTokenFile (file, info).empty());
    CHECK (api::runningInstance (file) == info);

    info.processId = 0xfffffff0u; // gone
    REQUIRE (api::writeTokenFile (file, info).empty());
    CHECK_FALSE (api::runningInstance (file).has_value());

    info = thisProcess();
    info.program = "hocket-headless.exe"; // the ID now belongs to another program
    REQUIRE (api::writeTokenFile (file, info).empty());
    CHECK_FALSE (api::runningInstance (file).has_value());
}

TEST_CASE ("a file that isn't a token file is ignored")
{
    const testing::TemporaryFolder folder (testing::unicodeName);
    const auto file = folder.path / "api.json";

    for (const auto* text : { "", "not json", "{}", R"({"port": "46253", "token": "x", "pid": 1, "program": "p"})" })
    {
        std::ofstream (file, std::ios::binary | std::ios::trunc) << text;
        CHECK_FALSE (api::readTokenFile (file).has_value());
    }
}
