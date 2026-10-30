#include "Console.h"

namespace hocket
{
void initialiseConsole()
{
    // POSIX terminals take UTF-8 as-is.
}

std::vector<std::string> utf8Arguments (int argc, char* argv[])
{
    // argv is UTF-8 on the platforms Hocket targets.
    return { argv, argv + argc };
}
}
