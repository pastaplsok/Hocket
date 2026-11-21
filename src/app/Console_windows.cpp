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
        if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
        {
            interrupted = true;
            return TRUE;
        }

        if (event == CTRL_CLOSE_EVENT)
        {
            // The console window is closing. Windows ends the process as soon as
            // this returns, or about 5 s after the window closed, whichever comes
            // first. So stop as for Ctrl+C, and don't return: the program stops
            // playback, closes the device, removes the token file and exits by
            // itself, well within the 5 s, and exiting ends this thread too.
            interrupted = true;
            Sleep (INFINITE);
        }

        return FALSE; // logging off or shutting down: the default, which ends the process
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

std::u32string typedKeys()
{
    std::u32string keys;
    const auto input = GetStdHandle (STD_INPUT_HANDLE);
    DWORD mode = 0;

    if (input == nullptr || input == INVALID_HANDLE_VALUE || ! GetConsoleMode (input, &mode))
        return keys; // not a console

    // The console's input records, read only while there are some, so it
    // never waits. With processed input (the default), Ctrl+C never gets here.
    for (DWORD waiting = 0; GetNumberOfConsoleInputEvents (input, &waiting) && waiting > 0;)
    {
        INPUT_RECORD records[16];
        DWORD read = 0;

        if (! ReadConsoleInputW (input, records, 16, &read) || read == 0)
            break;

        for (DWORD i = 0; i < read; ++i)
        {
            const auto& event = records[i].Event.KeyEvent;

            if (records[i].EventType == KEY_EVENT && event.bKeyDown && event.uChar.UnicodeChar != 0)
                keys += static_cast<char32_t> (event.uChar.UnicodeChar); // letters are never surrogates
        }
    }

    return keys;
}
}
