#pragma once

// Console setup for the command-line app. Platform-specific parts live in
// Console_<platform>.cpp.

namespace hocket
{
    // Makes the console show UTF-8 output correctly, and on Windows turns off
    // QuickEdit until the program exits, so a click in the console window can't
    // pause the program's output (and with it, --play's message thread). Call
    // once at startup.
    void initialiseConsole();

    // From now on, Ctrl+C (and Ctrl+Break on Windows) sets a flag instead of
    // ending the process, so the app can stop playback and close the audio
    // device cleanly. Poll interruptRequested() from the message thread.
    void catchInterrupts();
    bool interruptRequested();
}
