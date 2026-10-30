#include "platform/Platform.h"

#include "core/Paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

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
