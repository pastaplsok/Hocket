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
#include <io.h>

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

std::optional<std::filesystem::path> localAppDataFolder()
{
    PWSTR folder = nullptr;
    const auto result = SHGetKnownFolderPath (FOLDERID_LocalAppData, 0, nullptr, &folder);
    std::optional<std::filesystem::path> path;

    if (SUCCEEDED (result))
        path = std::filesystem::path (folder);

    CoTaskMemFree (folder);
    return path;
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
