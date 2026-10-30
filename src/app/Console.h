#pragma once

// Console and command-line setup for the command-line app. Platform-specific
// parts live in Console_<platform>.cpp.

#include <string>
#include <vector>

namespace hocket
{
    // Makes the console show UTF-8 output correctly. Call once at startup.
    void initialiseConsole();

    // The command-line arguments as UTF-8, program name first. Use these, not
    // argv: on Windows, argv is in the ANSI code page and loses most non-ASCII text.
    std::vector<std::string> utf8Arguments (int argc, char* argv[]);

    // From now on, Ctrl+C (and Ctrl+Break on Windows) sets a flag instead of
    // ending the process, so the app can stop playback and close the audio
    // device cleanly. Poll interruptRequested() from the message thread.
    void catchInterrupts();
    bool interruptRequested();
}
