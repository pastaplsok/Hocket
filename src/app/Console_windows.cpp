#include "Console.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>

namespace hocket
{
namespace
{
    std::atomic<bool> interrupted { false };

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
