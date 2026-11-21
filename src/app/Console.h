#pragma once

// Console setup for the command-line app. Platform-specific parts live in
// Console_<platform>.cpp.

#include <string>

namespace hocket
{
    // Makes the console show UTF-8 output correctly, and on Windows turns off
    // QuickEdit until the program exits, so a click in the console window can't
    // pause the program's output (and with it, --play's message thread). Call
    // once at startup.
    void initialiseConsole();

    // From now on, Ctrl+C (and Ctrl+Break on Windows) sets a flag instead of
    // ending the process, so the app can stop playback and close the audio
    // device cleanly. Poll interruptRequested() from the message thread. On
    // Windows, closing the console window sets it too, and the app then has
    // about 5 s to stop and exit before Windows ends it.
    void catchInterrupts();
    bool interruptRequested();

    // The characters typed in the console since the last call, without
    // waiting, echoing them, or needing Enter: for --play's keys. Empty when
    // nothing was typed, or the input isn't a console (it's redirected).
    // Ctrl+C isn't one: it still interrupts.
    std::u32string typedKeys();
}
