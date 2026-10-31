#pragma once

// hocket-stress: Milestone 0's stress test over the API (SPEC §10.0). It
// rewrites one clip in the running Hocket again and again at a steady rate,
// far faster than a model would, and reports how Hocket kept up.
//
// By default only a comment line changes, so the music should stay exactly
// the same: listening during the run, any click or gap is a glitch. Each
// rewrite still goes all the way through Hocket: validation, a history entry,
// and a rebuilt Tracktion sequence and playback graph. With vary, every other
// rewrite also moves the notes up an octave, so notes really come and go.
//
// When it's done, it writes the clip's original text back.

#include "api/Client.h"
#include "core/notes/Notes.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace hocket::stress
{
    struct Options
    {
        std::string clipId = "clp-bass";
        int count = 200;
        double perSecond = 20.0;
        bool vary = false;
        std::chrono::milliseconds replyTimeout = std::chrono::seconds (5);
    };

    // Connects to the Hocket the token file names and says hello. Returns an
    // error, or an empty string.
    std::string connect (api::Client& client, const std::filesystem::path& tokenFile, const std::string& name, const std::string& session);

    // The text of rewrite `index` (from 0) of `count`, made from the clip's own:
    // a comment line at the top says which one it is, so each differs from the
    // one before. With vary, odd ones also have every note an octave up (or
    // down, where up won't fit).
    std::string rewriteText (const notes::Clip& original, int index, int count, bool vary);

    struct Report
    {
        std::string error;                 // what stopped the run early, if anything
        int sent = 0;
        int applied = 0;                   // each added a history entry
        std::vector<std::string> failures; // the first few rewrites that didn't, and why
        std::vector<double> replySeconds;  // how long each answer took
        double seconds = 0.0;              // from the first send to the last answer
        double mostBehind = 0.0;           // the furthest a send started behind schedule, in seconds
        bool restored = false;             // the clip's original text is back
    };

    // Reads the clip, sends the rewrites on schedule, then writes the original
    // text back. Call it once connect() has succeeded.
    Report run (api::Client& client, const Options& options);

    // A few lines for a person: rates, answer times, failures, and whether the clip is restored.
    std::string summary (const Report& report, const Options& options);
}
