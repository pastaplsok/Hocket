#pragma once

// Console setup for the command-line app. Platform-specific parts live in
// Console_<platform>.cpp.

namespace hocket
{
    // Makes the console show UTF-8 output correctly. Call once at startup.
    void initialiseConsole();
}
