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

    // The user's Documents folder, as the OS reports it (on Windows it may
    // be redirected, to OneDrive say). nullopt if the OS doesn't say.
    std::optional<std::filesystem::path> documentsFolder();

    // The folder holding the running program's executable: where things
    // installed next to it (the bundled drum kits) are found.
    std::filesystem::path currentProgramFolder();

    // Writes `bytes` to `file`, replacing any file of that name, and returns
    // only once they're on the disk, so a crash or power cut afterwards can't
    // leave it empty or partial. Returns what went wrong, or empty; on failure
    // it leaves no file behind.
    std::string writeFileToDisk (const std::filesystem::path& file, std::string_view bytes);

    // Puts the file `from` in place of `to` in one step: anything opening `to`
    // finds the old file or the new one, never neither, nor part of one. `to`
    // need not exist. Returns once the move is on the disk; returns what went
    // wrong, or empty. On Windows it fails while another program has `to` open
    // without letting others delete it, as some editors do.
    std::string replaceFile (const std::filesystem::path& from, const std::filesystem::path& to);

    // Moves a file, but never over another: fails if `to` exists. Returns
    // what went wrong, or empty.
    std::string moveFile (const std::filesystem::path& from, const std::filesystem::path& to);

    // The longest path the OS takes without long-path support: 260 UTF-16
    // units on Windows, the terminating null included.
    inline constexpr std::size_t shortPathLimit = 260;

    // Whether this process can use paths longer than that. On Windows, only
    // when its manifest says it's long-path aware (cmake/LongPathAware.manifest)
    // and Windows' long-path setting (LongPathsEnabled) is on. Always true elsewhere.
    bool longPathsEnabled();

    // The CPU time the calling thread has used so far, user and kernel, in
    // seconds, precise to microseconds: next to wall-clock time, it tells a
    // thread that worked long from one the OS kept waiting. nullopt where it
    // can't be measured. On Windows the first call in a process takes about
    // 20 ms, measuring the CPU's cycle counter against the performance
    // counter, so make one before timing anything.
    std::optional<double> currentThreadCpuSeconds();

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
