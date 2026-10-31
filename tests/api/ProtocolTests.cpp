#include "api/Protocol.h"
#include "api/Tools.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace hocket;

TEST_CASE ("the build fingerprint is 16 hex digits, the same every time")
{
    const auto fingerprint = api::buildFingerprint();
    CHECK (fingerprint.size() == 16);
    CHECK (std::ranges::all_of (fingerprint, [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK (api::buildFingerprint() == fingerprint);
}

TEST_CASE ("begin_transaction's description gives the shared idle timeout")
{
    const auto tools = api::toolDefinitions (api::defaultIdleTimeout);
    const auto begin = std::ranges::find_if (tools, [] (const auto& tool) { return tool.at ("name") == "begin_transaction"; });
    REQUIRE (begin != tools.end());
    CHECK (begin->at ("description").template get<std::string>().find ("send no requests for 10 minutes") != std::string::npos);
}

TEST_CASE ("session IDs are 32 hex digits and never repeat")
{
    const auto id = api::makeSessionId();
    CHECK (id.size() == 32);
    CHECK (std::ranges::all_of (id, [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK (api::makeSessionId() != id);
}
