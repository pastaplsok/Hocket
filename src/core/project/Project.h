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
        midi, // clips are written with note names
        drums // a drums track: its drum map names its notes (SPEC §5.2)
    };

    // The longest a song can be (SPEC §4.4): every clip ends by the end of
    // this bar, so a typo in a clip's start can't make a loop thousands of bars long.
    inline constexpr int maxSongLengthInBars = 999;

    enum class DeviceType
    {
        synth,      // "hocket:synth": Hocket's synth, with Tracktion's 4OSC standing in until Milestone 4 (SPEC §8.1)
        drumSampler // "hocket:drum-sampler": Hocket's drum sampler, playing a kit (SPEC §8.4); on drums tracks only
    };

    struct Device
    {
        std::string id;
        DeviceType type = DeviceType::synth;
        std::string kit; // the drum sampler's kit, e.g. "acoustic"; empty for other devices

        bool operator== (const Device&) const = default;
    };

    // A clip's notes are its file's: clips naming the same file are linked,
    // and share them (SPEC §4.4). They're kept once, in Project::notesFiles.
    struct Clip
    {
        std::string id;
        std::string file;       // relative to the project folder, e.g. "clips/bass-verse.notes"
        notes::Ticks start = 0; // from the start of the song

        bool operator== (const Clip&) const = default;
    };

    struct Track
    {
        std::string id;
        std::string name;
        TrackType type = TrackType::midi;
        std::vector<Device> devices; // in signal-chain order; none, or one instrument
        std::vector<Clip> clips;

        // The mix (SPEC §4.4). Written in units: units::volume(), units::formatPan().
        double volume = 0.0; // dB, in steps of 0.1; 0 leaves the signal as it is, -infinity is silence
        int pan = 0;         // -100 (L100) to 100 (R100); a balance control, so 0 (C) leaves the signal as it is
        bool mute = false;
        bool solo = false;

        // A drums track's names for its notes (SPEC §5.2); empty on other tracks.
        notes::DrumMap drumMap;

        bool operator== (const Track&) const = default;
    };

    struct Project
    {
        double tempo = 120.0;      // quarter notes per minute, whatever the time signature
        notes::Meter meter;        // one time signature for the whole song
        std::vector<Track> tracks; // in display order

        // Each clip file's notes, by file ("clips/bass-verse.notes"). Every
        // clip's file is here, and nothing else is.
        std::map<std::string, notes::Clip, std::less<>> notesFiles;

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
    // "project.json" and each clip file, once however many clips use it. The
    // project must be valid.
    std::map<std::string, std::string> write (const Project& project);

    // A clip file's text in canonical form, as write() writes it; nullopt if
    // the project doesn't have the file.
    std::optional<std::string> clipFileText (const Project& project, std::string_view file);

    // "clips/bass.notes:6:10: message", "project.json: message", or just the message.
    std::string formatDiagnostic (const Diagnostic& diagnostic);

    // A clip's notes: its file's.
    const notes::Clip& notesOf (const Project& project, const Clip& clip);

    // Where a clip ends: its start plus its file's @length.
    notes::Ticks clipEnd (const Project& project, const Clip& clip);

    // The end of the last clip, rounded up to a bar line; at least one bar. The
    // song plays from 1.1 to here.
    notes::Ticks songEnd (const Project& project);

    // The clip with this ID, or nullptr. Sets *track to the clip's track if asked.
    const Clip* findClip (const Project& project, std::string_view id, const Track** track = nullptr);

    // The track with this ID, or nullptr.
    const Track* findTrack (const Project& project, std::string_view id);

    // What has this ID, for messages: "track 'trk-bass' (\"Bass\")", "clip
    // 'clp-a' on trk-bass", "device 'dev-bass-synth' on trk-bass"; nullopt
    // if nothing does. IDs are unique across the project (SPEC §4.1).
    std::optional<std::string> whatHasId (const Project& project, std::string_view id);

    // The clips whose notes are this file's, in track order, then clip order.
    std::vector<const Clip*> clipsUsing (const Project& project, std::string_view file);

    // The tracks with clips whose notes are this file's, in display order, each once.
    std::vector<const Track*> tracksUsing (const Project& project, std::string_view file);

    // A drums track's drum map; nullptr for a track that has none.
    const notes::DrumMap* drumMapOf (const Track& track);

    // The drum map a clip file is read and written with: its first track's.
    // Every track using a file names its notes alike (drumNameConflicts), so
    // any of them would write it the same way.
    const notes::DrumMap* drumMapOfFile (const Project& project, std::string_view file);

    // A file used on several tracks must mean the same notes on each (SPEC §5.2):
    // each of its notes has the same name in every track's drum map, or none.
    // Says where tracks a and b disagree on these notes, e.g. "note 36 (C2) is
    // 'kick' on trk-a but has no name on trk-b (its 'kick' is note 35)"; empty
    // when they agree.
    std::string drumNameConflicts (const notes::Clip& content, const Track& a, const Track& b);

    // Why `id` isn't a clip, for a tool's error message: "there's no clip 'clp-x';
    // the clips are 'clp-a', 'clp-b'", or a hint when it's a track or device ID.
    std::string unknownClipMessage (const Project& project, std::string_view id, std::string_view toolName);

    // Why `id` isn't a track: "there's no track 'trk-x'; the tracks are
    // 'trk-a', 'trk-b'", or a hint when it's a clip or device ID.
    std::string unknownTrackMessage (const Project& project, std::string_view id);

    // As written in project.json: "midi", "drums", "hocket:synth", "hocket:drum-sampler".
    const char* trackTypeName (TrackType type);
    const char* deviceTypeName (DeviceType type);
}
