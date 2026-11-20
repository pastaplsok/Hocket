#include "platform/Platform.h"

#include "core/Paths.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>

namespace hocket::platform
{
std::vector<std::string> utf8Arguments (int argc, char* argv[])
{
    // argv is UTF-8 on the platforms Hocket targets.
    return { argv, argv + argc };
}

void useBinaryStandardStreams()
{
    // POSIX streams never translate line endings.
}

bool secureRandomBytes (std::span<std::uint8_t> bytes)
{
    std::ifstream random ("/dev/urandom", std::ios::binary);
    return random.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size())).good();
}

std::uint32_t currentProcessId()
{
    return static_cast<std::uint32_t> (getpid());
}

std::string currentProgramName()
{
    std::error_code error;
    return utf8FromPath (std::filesystem::read_symlink (pathFromUtf8 ("/proc/self/exe"), error).filename());
}

bool isProgramRunning (std::uint32_t processId, std::string_view programName)
{
    if (kill (static_cast<pid_t> (processId), 0) != 0)
        return false;

    // Linux names a process's program in /proc; elsewhere, trust the process ID.
    std::error_code error;
    const auto program = std::filesystem::read_symlink (pathFromUtf8 ("/proc/" + std::to_string (processId) + "/exe"), error);
    return error || utf8FromPath (program.filename()) == programName;
}

std::optional<std::filesystem::path> localAppDataFolder()
{
    if (const auto* state = std::getenv ("XDG_STATE_HOME"); state != nullptr && *state != '\0')
        return pathFromUtf8 (state);

    if (const auto* home = std::getenv ("HOME"); home != nullptr && *home != '\0')
        return pathFromUtf8 (home) / pathFromUtf8 (".local/state");

    return {};
}

std::optional<std::filesystem::path> documentsFolder()
{
    // XDG_DOCUMENTS_DIR lives in user-dirs.dirs, not the environment; the
    // ports will read it. Until then, ~/Documents.
    if (const auto* home = std::getenv ("HOME"); home != nullptr && *home != '\0')
        return pathFromUtf8 (home) / pathFromUtf8 ("Documents");

    return {};
}

std::filesystem::path currentProgramFolder()
{
    std::error_code error;
    return std::filesystem::read_symlink ("/proc/self/exe", error).parent_path();
}

namespace
{
    std::string errnoMessage()
    {
        return std::strerror (errno);
    }

    // Makes a rename in this folder durable: the folder's entry is on the disk too.
    void syncFolder (const std::filesystem::path& folder)
    {
        const auto descriptor = open (folder.empty() ? "." : folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

        if (descriptor >= 0)
        {
            fsync (descriptor);
            close (descriptor);
        }
    }
}

std::string writeFileToDisk (const std::filesystem::path& file, std::string_view bytes)
{
    const auto descriptor = open (file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (descriptor < 0)
        return errnoMessage();

    std::string error;
    auto remaining = bytes;

    while (error.empty() && ! remaining.empty())
    {
        const auto written = write (descriptor, remaining.data(), remaining.size());

        if (written < 0 && errno != EINTR)
            error = errnoMessage();
        else if (written > 0)
            remaining.remove_prefix (static_cast<std::size_t> (written));
    }

    if (error.empty() && fsync (descriptor) != 0)
        error = errnoMessage();

    close (descriptor);

    if (! error.empty())
        unlink (file.c_str());

    return error;
}

std::string replaceFile (const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (rename (from.c_str(), to.c_str()) != 0)
        return errnoMessage();

    syncFolder (to.parent_path());
    return {};
}

std::string moveFile (const std::filesystem::path& from, const std::filesystem::path& to)
{
    // A hard link fails if `to` exists, so nothing is ever moved over another file.
    if (link (from.c_str(), to.c_str()) != 0)
        return errnoMessage();

    unlink (from.c_str());
    syncFolder (to.parent_path());
    return {};
}

std::optional<FileLock> FileLock::acquire (const std::filesystem::path& file)
{
    const auto descriptor = open (file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (descriptor < 0)
        return {};

    // Released when the descriptor closes, which the OS does when the process ends.
    if (flock (descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        close (descriptor);
        return {};
    }

    return FileLock (descriptor);
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
        close (static_cast<int> (std::exchange (handle, none)));
}

bool isTcpPortFree (int port)
{
    const auto probe = socket (AF_INET, SOCK_STREAM, 0);

    if (probe < 0)
        return false;

    // SO_REUSEADDR here only lets the bind past connections in TIME_WAIT; it
    // still fails if anything is listening on the port.
    int reuse = 1;
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons (static_cast<std::uint16_t> (port));
    address.sin_addr.s_addr = htonl (INADDR_ANY);

    const auto free = setsockopt (probe, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse)) == 0
                   && bind (probe, reinterpret_cast<const sockaddr*> (&address), sizeof (address)) == 0;

    close (probe);
    return free;
}
}
