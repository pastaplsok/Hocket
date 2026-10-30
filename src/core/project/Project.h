#pragma once

// A Hocket project (SPEC §4): the in-memory model, loading a project folder
// (project.json plus each clip's .notes file), and writing it in canonical form.

#include "core/notes/Notes.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::project
{
    // Version 0 is the pre-release format: it may change without migrations until v1 (SPEC §4.1).
    inline constexpr int formatVersion = 0;

    enum class TrackType
    {
        midi
    };

    enum class DeviceType
    {
        fourOsc // Tracktion's built-in 4OSC synth, Milestone 0's stand-in (SPEC §10.0)
    };

    struct Device
    {
        std::string id;
        DeviceType type = DeviceType::fourOsc;

        bool operator== (const Device&) const = default;
    };

    struct Clip
    {
        std::string id;
        std::string file;       // relative to the project folder, e.g. "clips/bass-verse.notes"
        notes::Ticks start = 0; // from the start of the song
        notes::Clip content;    // the parsed .notes file

        bool operator== (const Clip&) const = default;
    };

    struct Track
    {
        std::string id;
        std::string name;
        TrackType type = TrackType::midi;
        std::vector<Device> devices; // in signal-chain order
        std::vector<Clip> clips;

        bool operator== (const Track&) const = default;
    };

    struct Project
    {
        double tempo = 120.0; // BPM
        notes::Meter meter;   // 4/4 only in Milestone 0
        std::vector<Track> tracks; // in display order

        bool operator== (const Project&) const = default;
    };

    struct Diagnostic
    {
        std::string file;   // relative to the project folder; empty for problems with the folder itself
        int line = 0;       // 1-based; 0 when the problem has no place in the file
        int column = 0;
        std::string message;

        bool operator== (const Diagnostic&) const = default;
    };

    struct LoadResult
    {
        std::optional<Project> project; // set only when there are no errors
        std::vector<Diagnostic> errors; // project.json's first, then each clip file's
    };

    // Returns a file's contents, or nullopt if it doesn't exist. Paths are
    // relative to the project folder, with forward slashes.
    using FileReader = std::function<std::optional<std::string> (const std::string& relativePath)>;

    // Validates project.json and reads each clip's .notes file through readFile.
    LoadResult parse (std::string_view projectJson, const FileReader& readFile);

    // Loads a project folder from disk.
    LoadResult load (const std::filesystem::path& folder);

    // Every file of the project in canonical form, keyed by relative path:
    // "project.json" and each clip's .notes file. The project must be valid.
    std::map<std::string, std::string> write (const Project& project);

    // "clips/bass.notes:6:10: message", "project.json: message", or just the message.
    std::string formatDiagnostic (const Diagnostic& diagnostic);
}
