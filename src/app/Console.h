#pragma once

// Console setup for the command-line app. Platform-specific parts live in
// Console_<platform>.cpp.

namespace hocket
{
    // Makes the console show UTF-8 output correctly. Call once at startup.
    void initialiseConsole();

    // From now on, Ctrl+C (and Ctrl+Break on Windows) sets a flag instead of
    // ending the process, so the app can stop playback and close the audio
    // device cleanly. Poll interruptRequested() from the message thread.
    void catchInterrupts();
    bool interruptRequested();
}
