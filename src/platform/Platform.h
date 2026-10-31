#pragma once

// Operating-system services, behind one interface: each platform has its own
// Platform_<name>.cpp. Text in and out is UTF-8; paths are std::filesystem::path.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::platform
{
    // The command-line arguments as UTF-8, program name first. Use these, not
    // argv: on Windows, argv is in the ANSI code page and loses most non-ASCII text.
    std::vector<std::string> utf8Arguments (int argc, char* argv[]);

    // Makes stdin and stdout pass bytes through unchanged. On Windows, text
    // mode turns "\n" into "\r\n" on the way out and back on the way in.
    void useBinaryStandardStreams();

    // Fills `bytes` from the OS's cryptographically secure random source
    // (BCryptGenRandom on Windows). False if it fails.
    bool secureRandomBytes (std::span<std::uint8_t> bytes);

    std::uint32_t currentProcessId();

    // The running program's file name, e.g. "hocket-headless.exe".
    std::string currentProgramName();

    // Whether the process with this ID is running the program with this file
    // name. Checking the name means a crashed process's ID, reused by another
    // program, doesn't count.
    bool isProgramRunning (std::uint32_t processId, std::string_view programName);

    // The per-user folder for app data that stays on this machine:
    // %LOCALAPPDATA% on Windows. nullopt if the OS doesn't say.
    std::optional<std::filesystem::path> localAppDataFolder();

    // A file held so that no other holder can take it until this one lets go
    // or its process ends, crashes included: for "only one at a time". On
    // Windows nothing else can open the file meanwhile, not even to read it;
    // elsewhere it's an advisory lock (flock).
    class FileLock
    {
    public:
        // Takes the lock, creating the file if needed (but not its folder).
        // nullopt if something else holds it, or the file can't be opened.
        static std::optional<FileLock> acquire (const std::filesystem::path& file);

        FileLock (FileLock&& other) noexcept;
        FileLock& operator= (FileLock&& other) noexcept;
        ~FileLock();

        FileLock (const FileLock&) = delete;
        FileLock& operator= (const FileLock&) = delete;

    private:
        static constexpr std::intptr_t none = -1;

        explicit FileLock (std::intptr_t heldHandle) : handle (heldHandle) {}
        void release();

        std::intptr_t handle = none; // a HANDLE on Windows, a file descriptor elsewhere
    };

    // Whether no program is listening on this TCP port, on 127.0.0.1 or on all
    // addresses. Connections left in TIME_WAIT don't count.
    //
    // IXWebSocket's server sets SO_REUSEADDR, which on Windows lets it bind a
    // port another program is already listening on, so its own bind can't tell.
    bool isTcpPortFree (int port);
}
