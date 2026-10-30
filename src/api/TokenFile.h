#pragma once

// The token file (SPEC §6.3): how a client finds the running DAW's API and
// proves it may use it. The DAW writes it when the API starts and deletes it
// on exit; hocket-mcp reads it. It lives in the user's own app data folder,
// which other users can't read.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace hocket::api
{
    struct ApiInfo
    {
        int port = 0;
        std::string token;
        std::uint32_t processId = 0;
        std::string program; // the DAW's file name, e.g. "hocket-headless.exe"

        bool operator== (const ApiInfo&) const = default;
    };

    // %LOCALAPPDATA%\Hocket\api.json on Windows; nullopt if the OS has no such folder.
    std::optional<std::filesystem::path> defaultTokenFilePath();

    // 32 bytes from the OS's secure random source, as 64 hex digits; empty if
    // the OS can't supply them.
    std::string makeToken();

    // The file's contents; nullopt if it's missing or isn't a token file.
    std::optional<ApiInfo> readTokenFile (const std::filesystem::path& file);

    // Writes the file atomically (a temporary file, then a rename), creating its
    // folder if needed. Returns an error, or an empty string.
    std::string writeTokenFile (const std::filesystem::path& file, const ApiInfo& info);

    // Deletes the file if it still holds `info`, so another instance's is never removed.
    void removeTokenFile (const std::filesystem::path& file, const ApiInfo& info);

    // What the file says, if the program it names is still running. A file left
    // behind by a crash doesn't count.
    std::optional<ApiInfo> runningInstance (const std::filesystem::path& file);
}
