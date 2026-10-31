#pragma once

// What both ends of the WebSocket API must agree on (SPEC §6.3). The DAW and
// hocket-mcp compile in the same tool definitions (Tools.h), and hocket-mcp
// serves them itself, so a client sees the tools before Hocket is running.
// hello carries the client's version and build fingerprint, and the DAW
// refuses a client from another build, so the tools a client was shown are
// the ones the DAW runs.

#include <chrono>
#include <string>
#include <string_view>

namespace hocket::api
{
    // Bump it when hello, the messages or their formats change in a way the
    // tool definitions don't show.
    constexpr int protocolVersion = 1;

    // How long a transaction stays open without a request from its owner
    // (SPEC §3.1). begin_transaction's description gives it, so the DAW and
    // hocket-mcp must use the same value.
    constexpr std::chrono::seconds defaultIdleTimeout = std::chrono::minutes (10);

    // Hocket's version, e.g. "0.0.1". It stays the same through development,
    // so the fingerprint is what tells two builds apart.
    std::string_view hocketVersion();

    // A hash of protocolVersion and the tool definitions (with the default idle
    // timeout), as 16 hex digits. Builds with the same fingerprint speak the same API.
    std::string buildFingerprint();
}
