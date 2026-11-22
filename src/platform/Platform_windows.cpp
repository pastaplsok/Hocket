#include "platform/Platform.h"

#include "core/Paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <fcntl.h>
#include <intrin.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace hocket::platform
{
namespace
{
    // Closes a handle when it goes out of scope.
    struct ProcessHandle
    {
        HANDLE handle;

        ~ProcessHandle()
        {
            if (handle != nullptr)
                CloseHandle (handle);
        }
    };

    std::filesystem::path programPath (HANDLE process)
    {
        std::vector<wchar_t> buffer (MAX_PATH);

        for (;;)
        {
            auto size = static_cast<DWORD> (buffer.size());

            if (QueryFullProcessImageNameW (process, 0, buffer.data(), &size))
                return std::filesystem::path (std::wstring (buffer.data(), size));

            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() > 32768)
                return {};

            buffer.resize (buffer.size() * 2);
        }
    }

    bool sameFileName (const std::filesystem::path& a, const std::filesystem::path& b)
    {
        // File names on Windows are case-insensitive.
        const auto& x = a.native();
        const auto& y = b.native();
        return CompareStringOrdinal (x.data(), static_cast<int> (x.size()), y.data(), static_cast<int> (y.size()), TRUE) == CSTR_EQUAL;
    }

    std::string utf8FromWide (const wchar_t* text)
    {
        const auto size = WideCharToMultiByte (CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

        if (size <= 1)
            return {};

        std::string utf8 (static_cast<std::size_t> (size - 1), '\0');
        WideCharToMultiByte (CP_UTF8, 0, text, -1, utf8.data(), size, nullptr, nullptr);
        return utf8;
    }
}

std::vector<std::string> utf8Arguments (int, char*[])
{
    // argv is already lossy here, so read the UTF-16 command line instead.
    int count = 0;
    auto** wide = CommandLineToArgvW (GetCommandLineW(), &count);
    std::vector<std::string> arguments;

    if (wide == nullptr)
        return arguments;

    for (int i = 0; i < count; ++i)
        arguments.push_back (utf8FromWide (wide[i]));

    LocalFree (wide);
    return arguments;
}

void useBinaryStandardStreams()
{
    _setmode (_fileno (stdin), _O_BINARY);
    _setmode (_fileno (stdout), _O_BINARY);
}

bool secureRandomBytes (std::span<std::uint8_t> bytes)
{
    return BCryptGenRandom (nullptr, bytes.data(), static_cast<ULONG> (bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

std::uint32_t currentProcessId()
{
    return GetCurrentProcessId();
}

std::string currentProgramName()
{
    return utf8FromPath (programPath (GetCurrentProcess()).filename());
}

bool isProgramRunning (std::uint32_t processId, std::string_view programName)
{
    const ProcessHandle process { OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId) };

    if (process.handle == nullptr || WaitForSingleObject (process.handle, 0) != WAIT_TIMEOUT)
        return false;

    return sameFileName (programPath (process.handle).filename(), pathFromUtf8 (programName));
}

namespace
{
    std::optional<std::filesystem::path> knownFolder (REFKNOWNFOLDERID id)
    {
        PWSTR folder = nullptr;
        const auto result = SHGetKnownFolderPath (id, 0, nullptr, &folder);
        std::optional<std::filesystem::path> path;

        if (SUCCEEDED (result))
            path = std::filesystem::path (folder);

        CoTaskMemFree (folder);
        return path;
    }
}

std::optional<std::filesystem::path> localAppDataFolder()
{
    return knownFolder (FOLDERID_LocalAppData);
}

std::optional<std::filesystem::path> documentsFolder()
{
    return knownFolder (FOLDERID_Documents);
}

std::filesystem::path currentProgramFolder()
{
    return programPath (GetCurrentProcess()).parent_path();
}

namespace
{
    // The system's message for the last error, e.g. "The process cannot
    // access the file because it is being used by another process."
    std::string lastErrorMessage()
    {
        const auto code = GetLastError();
        wchar_t* text = nullptr;
        const auto length = FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                                            0, reinterpret_cast<wchar_t*> (&text), 0, nullptr);

        std::string message;

        if (length > 0 && text != nullptr)
            message = utf8FromWide (text);

        LocalFree (text);

        while (! message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
            message.pop_back();

        return message.empty() ? "Windows error " + std::to_string (code) : message;
    }
}

std::string writeFileToDisk (const std::filesystem::path& file, std::string_view bytes)
{
    const auto handle = CreateFileW (file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        return lastErrorMessage();

    std::string error;
    auto remaining = bytes;

    while (error.empty() && ! remaining.empty())
    {
        const auto chunk = static_cast<DWORD> (std::min<std::size_t> (remaining.size(), 1u << 30));
        DWORD written = 0;

        if (! WriteFile (handle, remaining.data(), chunk, &written, nullptr) || written == 0)
            error = lastErrorMessage();
        else
            remaining.remove_prefix (written);
    }

    // Until this, the bytes may be only in the system's cache.
    if (error.empty() && ! FlushFileBuffers (handle))
        error = lastErrorMessage();

    CloseHandle (handle);

    if (! error.empty())
        DeleteFileW (file.c_str());

    return error;
}

namespace
{
    // After a rename within a volume, which Windows only journals:
    // MOVEFILE_WRITE_THROUGH is documented for moves done as a copy and a
    // delete, between volumes. Flushing the file or folder at its new name
    // writes NTFS's journal out up to the rename, so it's on the disk when
    // this returns. Best effort: one that can't be opened to flush (another
    // program holding it) is left to the journal's own timing, a few seconds.
    // It changes no file's date.
    void flushRenamed (const std::filesystem::path& path)
    {
        const auto handle = CreateFileW (path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);

        if (handle == INVALID_HANDLE_VALUE)
            return;

        FlushFileBuffers (handle);
        CloseHandle (handle);
    }
}

std::string replaceFile (const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (! MoveFileExW (from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return lastErrorMessage();

    flushRenamed (to);
    return {};
}

std::string moveFile (const std::filesystem::path& from, const std::filesystem::path& to)
{
    // Without MOVEFILE_REPLACE_EXISTING, it fails if `to` exists.
    if (! MoveFileExW (from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
        return lastErrorMessage();

    flushRenamed (to);
    return {};
}

bool longPathsEnabled()
{
    // In ntdll since Windows 10 1607; true only when both the manifest and
    // the system setting allow long paths for this process.
    using Function = BOOLEAN (NTAPI*)();
    static const auto function = reinterpret_cast<Function> (reinterpret_cast<void*> (GetProcAddress (GetModuleHandleW (L"ntdll.dll"), "RtlAreLongPathsEnabled")));
    return function != nullptr && function() != FALSE;
}

namespace
{
    // Cycles of the CPU's time-stamp counter per second, which
    // QueryThreadCycleTime counts in. Windows doesn't say, so it's measured
    // against the performance counter over 20 ms; the counter runs at a
    // constant rate on the CPUs Windows 11 supports. 0 if it can't be.
    double cycleCounterRate()
    {
#if defined(_M_X64) || defined(_M_IX86)
        LARGE_INTEGER frequency, start, end;

        if (! QueryPerformanceFrequency (&frequency) || ! QueryPerformanceCounter (&start))
            return 0.0;

        const auto startCycles = __rdtsc();
        Sleep (20);
        const auto endCycles = __rdtsc();
        QueryPerformanceCounter (&end);

        const auto seconds = static_cast<double> (end.QuadPart - start.QuadPart) / static_cast<double> (frequency.QuadPart);
        return seconds > 0.0 ? static_cast<double> (endCycles - startCycles) / seconds : 0.0;
#else
        return 0.0;
#endif
    }
}

std::optional<double> currentThreadCpuSeconds()
{
    // GetThreadTimes is only updated at the system timer's tick (15.6 ms by
    // default), too coarse for audio blocks; the cycle count is exact.
    static const auto rate = cycleCounterRate();
    ULONG64 cycles = 0;

    if (rate <= 0.0 || ! QueryThreadCycleTime (GetCurrentThread(), &cycles))
        return {};

    return static_cast<double> (cycles) / rate;
}

std::optional<FileLock> FileLock::acquire (const std::filesystem::path& file)
{
    // No sharing at all: while it's open here, any other open fails. Windows
    // closes it when the process ends, however it ends.
    const auto held = CreateFileW (file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (held == INVALID_HANDLE_VALUE)
        return {};

    return FileLock (reinterpret_cast<std::intptr_t> (held));
}

FileLock::FileLock (FileLock&& other) noexcept : handle (std::exchange (other.handle, none)) {}

FileLock& FileLock::operator= (FileLock&& other) noexcept
{
    if (this != &other)
    {
        release();
        handle = std::exchange (other.handle, none);
    }

    return *this;
}

FileLock::~FileLock()
{
    release();
}

void FileLock::release()
{
    if (handle != none)
        CloseHandle (reinterpret_cast<HANDLE> (std::exchange (handle, none)));
}

bool isTcpPortFree (int port)
{
    WSADATA data;

    if (WSAStartup (MAKEWORD (2, 2), &data) != 0)
        return false;

    // An exclusive bind to all addresses fails if any program is listening on
    // the port, on any address and with any options, but not because of
    // connections in TIME_WAIT. A plain bind misses a listener using
    // SO_REUSEADDR or one on all addresses.
    auto free = false;
    const auto probe = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (probe != INVALID_SOCKET)
    {
        BOOL exclusive = TRUE;
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons (static_cast<u_short> (port));
        address.sin_addr.s_addr = htonl (INADDR_ANY);

        free = setsockopt (probe, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*> (&exclusive), sizeof (exclusive)) == 0
            && bind (probe, reinterpret_cast<const sockaddr*> (&address), sizeof (address)) == 0;

        closesocket (probe);
    }

    WSACleanup();
    return free;
}
}
