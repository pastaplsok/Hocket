#pragma once

// What changed in the project, as the command bus announces it after each
// command, undo, redo or rollback, so the engine can update just that (SPEC §3.3).

#include <string>
#include <variant>

namespace hocket::commands
{
    // A clip file's notes changed, or the file joined or left the project.
    // Every clip using the file changed.
    struct NotesChanged
    {
        std::string file;
    };

    struct TempoChanged
    {
    };

    // Clips and notes keep their times, so their starts are positions in the
    // new signature; each file's @length changed too, with a NotesChanged.
    struct TimeSignatureChanged
    {
    };

    // With its clips, which get no ClipAdded of their own.
    struct TrackAdded
    {
        std::string trackId;
    };

    // With its clips, which get no ClipRemoved of their own.
    struct TrackRemoved
    {
        std::string trackId;
    };

    // To another place in the display order.
    struct TrackMoved
    {
        std::string trackId;
    };

    struct TrackRenamed
    {
        std::string trackId;
    };

    // Any of its volume, pan, mute and solo.
    struct MixChanged
    {
        std::string trackId;
    };

    // Its devices: an instrument added, removed or replaced, or a drum sampler's kit.
    struct InstrumentChanged
    {
        std::string trackId;
    };

    // How the track's clips are written, not what they play.
    struct DrumMapChanged
    {
        std::string trackId;
    };

    struct ClipAdded
    {
        std::string clipId;
    };

    struct ClipRemoved
    {
        std::string clipId;
        std::string trackId; // the track it was on
    };

    // A new start on the same track. A clip moved to another track is
    // removed from one and added to the other.
    struct ClipMoved
    {
        std::string clipId;
    };

    // It plays another file's notes now, e.g. a copy of its own, once it was
    // made unique. What it plays is the same until that file is written.
    struct ClipFileChanged
    {
        std::string clipId;
    };

    using Change = std::variant<NotesChanged, TempoChanged, TimeSignatureChanged, TrackAdded, TrackRemoved, TrackMoved, TrackRenamed,
                                MixChanged, InstrumentChanged, DrumMapChanged, ClipAdded, ClipRemoved, ClipMoved, ClipFileChanged>;
}
