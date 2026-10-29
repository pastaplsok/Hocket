#include "Console.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace hocket
{
    void initialiseConsole()
    {
        // Device names and project text are UTF-8; the default console code page isn't.
        SetConsoleOutputCP (CP_UTF8);
    }
}
