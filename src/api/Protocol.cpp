#include "api/Protocol.h"
#include "api/Tools.h"

#include <cstdint>

namespace hocket::api
{
std::string_view hocketVersion()
{
    return HOCKET_VERSION;
}

std::string buildFingerprint()
{
    // FNV-1a, 64-bit: this only has to notice a change, not resist one.
    const auto text = "protocol " + std::to_string (protocolVersion) + "\n" + toolDefinitions (defaultIdleTimeout).dump();
    std::uint64_t hash = 0xcbf29ce484222325;

    for (const auto c : text)
    {
        hash ^= static_cast<unsigned char> (c);
        hash *= 0x100000001b3;
    }

    constexpr std::string_view digits = "0123456789abcdef";
    std::string fingerprint (16, '0');

    for (auto i = fingerprint.size(); i-- > 0; hash >>= 4)
        fingerprint[i] = digits[hash & 0xf];

    return fingerprint;
}
}
