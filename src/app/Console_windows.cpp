#include "Console.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

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

void initialiseConsole()
{
    // Device names and project text are UTF-8; the default console code page isn't.
    SetConsoleOutputCP (CP_UTF8);
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

void catchInterrupts()
{
    SetConsoleCtrlHandler (handleConsoleControl, TRUE);
}

bool interruptRequested()
{
    return interrupted;
}
}
