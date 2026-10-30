#include "Console.h"

#include <atomic>
#include <csignal>

namespace hocket
{
namespace
{
    // Lock-free, so the signal handler may set it.
    std::atomic<bool> interrupted { false };

    void handleInterrupt (int)
    {
        interrupted = true;
    }
}

void initialiseConsole()
{
    // POSIX terminals take UTF-8 as-is.
}

std::vector<std::string> utf8Arguments (int argc, char* argv[])
{
    // argv is UTF-8 on the platforms Hocket targets.
    return { argv, argv + argc };
}

void catchInterrupts()
{
    std::signal (SIGINT, handleInterrupt);
}

bool interruptRequested()
{
    return interrupted;
}
}
