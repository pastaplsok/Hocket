#include "Console.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdlib>

namespace hocket
{
namespace
{
    std::atomic<bool> interrupted { false };

    DWORD inputModeAtStart = 0;

    void restoreInputMode()
    {
        SetConsoleMode (GetStdHandle (STD_INPUT_HANDLE), inputModeAtStart);
    }

    // Windows calls this on a thread of its own.
    BOOL WINAPI handleConsoleControl (DWORD event)
    {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
            return FALSE; // closing the console window etc.: the default, which ends the process

        interrupted = true;
        return TRUE;
    }
}

void initialiseConsole()
{
    // Device names and project text are UTF-8; the default console code page isn't.
    SetConsoleOutputCP (CP_UTF8);

    // With QuickEdit on, a click in the console window starts a selection, and
    // while there is one, writing to the console blocks. --play logs from its
    // message thread, so the API and --watch would stop until Esc. The mode
    // belongs to the console, not this program, so it's put back at exit.
    const auto input = GetStdHandle (STD_INPUT_HANDLE);
    DWORD mode = 0;

    if (input != nullptr && input != INVALID_HANDLE_VALUE && GetConsoleMode (input, &mode) && (mode & ENABLE_QUICK_EDIT_MODE) != 0)
    {
        if (SetConsoleMode (input, (mode | ENABLE_EXTENDED_FLAGS) & ~static_cast<DWORD> (ENABLE_QUICK_EDIT_MODE)))
        {
            inputModeAtStart = mode;
            std::atexit (restoreInputMode);
        }
    }
}

void catchInterrupts()
{
    SetConsoleCtrlHandler (handleConsoleControl, TRUE);
}

bool interruptRequested()
{
    return interrupted;
}
}
