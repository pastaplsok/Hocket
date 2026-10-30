#include "api/TokenFile.h"

#include "core/Paths.h"
#include "platform/Platform.h"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>

namespace hocket::api
{
namespace
{
    using Json = nlohmann::json;

    constexpr std::size_t tokenBytes = 32;
}

std::optional<std::filesystem::path> defaultTokenFilePath()
{
    const auto folder = platform::localAppDataFolder();

    if (! folder)
        return {};

    return *folder / "Hocket" / "api.json";
}

std::string makeToken()
{
    std::array<std::uint8_t, tokenBytes> bytes {};

    if (! platform::secureRandomBytes (bytes))
        return {};

    constexpr std::string_view digits = "0123456789abcdef";
    std::string token;

    for (const auto byte : bytes)
    {
        token += digits[byte >> 4];
        token += digits[byte & 0xf];
    }

    return token;
}

std::optional<ApiInfo> readTokenFile (const std::filesystem::path& file)
{
    const auto text = readTextFile (file);

    if (! text)
        return {};

    const auto json = Json::parse (*text, nullptr, false);

    if (! json.is_object()
        || ! json.contains ("port") || ! json.at ("port").is_number_integer()
        || ! json.contains ("token") || ! json.at ("token").is_string()
        || ! json.contains ("pid") || ! json.at ("pid").is_number_unsigned()
        || ! json.contains ("program") || ! json.at ("program").is_string())
        return {};

    return ApiInfo { json.at ("port").get<int>(), json.at ("token").get<std::string>(),
                     json.at ("pid").get<std::uint32_t>(), json.at ("program").get<std::string>() };
}

std::string writeTokenFile (const std::filesystem::path& file, const ApiInfo& info)
{
    const Json json { { "port", info.port }, { "token", info.token }, { "pid", info.processId }, { "program", info.program } };

    std::error_code error;
    std::filesystem::create_directories (file.parent_path(), error);

    if (error)
        return "couldn't create the folder " + utf8FromPath (file.parent_path()) + ": " + error.message();

    auto temporary = file;
    temporary += ".tmp-" + std::to_string (info.processId);

    {
        std::ofstream out (temporary, std::ios::binary | std::ios::trunc);
        out << json.dump (2) << '\n';

        if (! out.flush())
            return "couldn't write " + utf8FromPath (temporary);
    }

    std::filesystem::rename (temporary, file, error);

    if (error)
    {
        std::filesystem::remove (temporary, error);
        return "couldn't replace " + utf8FromPath (file) + ": " + error.message();
    }

    return {};
}

void removeTokenFile (const std::filesystem::path& file, const ApiInfo& info)
{
    if (readTokenFile (file) == info)
    {
        std::error_code error;
        std::filesystem::remove (file, error);
    }
}

std::optional<ApiInfo> runningInstance (const std::filesystem::path& file)
{
    auto info = readTokenFile (file);

    if (! info || ! platform::isProgramRunning (info->processId, info->program))
        return {};

    return info;
}
}
