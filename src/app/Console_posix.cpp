#include "Console.h"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>

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

    termios terminalAtStart {};

    void restoreTerminal()
    {
        tcsetattr (STDIN_FILENO, TCSANOW, &terminalAtStart);
    }

    // Keys arrive as they're typed, unechoed, from the first call on; put back at exit.
    bool keysAsTyped()
    {
        static const bool set = []
        {
            if (! isatty (STDIN_FILENO) || tcgetattr (STDIN_FILENO, &terminalAtStart) != 0)
                return false;

            auto raw = terminalAtStart;
            raw.c_lflag &= static_cast<tcflag_t> (~(ICANON | ECHO)); // ISIG stays, so Ctrl+C still interrupts
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;

            if (tcsetattr (STDIN_FILENO, TCSANOW, &raw) != 0)
                return false;

            std::atexit (restoreTerminal);
            return true;
        }();

        return set;
    }
}

void initialiseConsole()
{
    // POSIX terminals take UTF-8 as-is.
}

void catchInterrupts()
{
    std::signal (SIGINT, handleInterrupt);
}

bool interruptRequested()
{
    return interrupted;
}

std::u32string typedKeys()
{
    std::u32string keys;

    if (! keysAsTyped())
        return keys;

    // ASCII only: --play's keys are letters. (Never compiled yet, like the rest of the POSIX code.)
    for (pollfd waiting { STDIN_FILENO, POLLIN, 0 }; poll (&waiting, 1, 0) > 0 && (waiting.revents & POLLIN) != 0;)
    {
        unsigned char byte = 0;

        if (read (STDIN_FILENO, &byte, 1) != 1)
            break;

        if (byte < 0x80)
            keys += static_cast<char32_t> (byte);
    }

    return keys;
}
}
