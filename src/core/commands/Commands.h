#pragma once

// The commands clients send to the command bus (SPEC §3.1), who sends them,
// and what comes back.
//
// Values are written as project.json and .notes write them: positions like
// "9.1", volumes like "-6dB", so the bus validates them with the format's own
// parsers, and its errors are the format's.

#include "core/project/Rules.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hocket::commands
{
    // Replaces a clip's notes with new .notes text (SPEC §6.2). The notes are
    // its file's, so clips linked to it change too. A new @length resizes them all.
    struct WriteClip
    {
        std::string clipId;
        std::string notes;
    };

    //==============================================================================
    // The song's tempo, in quarter notes per minute (SPEC §4.4).
    struct SetTempo
    {
        double bpm = 120.0;
    };

    // "6/8". Every clip and note keeps its time (SPEC §4.4), so it's refused
    // unless every clip is a whole number of the new bars.
    struct SetTimeSignature
    {
        std::string timeSignature;
    };

    //==============================================================================
    // Where a track goes in the display order: before or after another track,
    // by ID, never by index (SPEC §4.1). Give one or neither.
    struct TrackPlace
    {
        std::string before;
        std::string after;
    };

    // A track's instrument (SPEC §4.4): "hocket:synth", or "hocket:drum-sampler"
    // with its kit, on drums tracks only.
    struct Instrument
    {
        std::string type;
        std::string kit;
    };

    struct AddTrack
    {
        std::string type;                     // "midi" or "drums"; a drums track gets the standard drum map
        std::string name;                     // "Drums"
        std::string id;                       // optional: Hocket makes one from the name
        TrackPlace place;                     // neither: at the end
        std::optional<Instrument> instrument; // none if not given
    };

    // With its clips. A file no other clip uses leaves the project.
    struct RemoveTrack
    {
        std::string trackId;
    };

    struct RenameTrack
    {
        std::string trackId;
        std::string name;
    };

    // Give one place: to move a track last, put it after the last track.
    struct MoveTrack
    {
        std::string trackId;
        TrackPlace place;
    };

    // Any of a track's volume ("-6dB"), pan ("L20"), mute and solo; at least one.
    struct SetMix
    {
        std::string trackId;
        std::optional<std::string> volume;
        std::optional<std::string> pan;
        std::optional<bool> mute;
        std::optional<bool> solo;
    };

    // A track's instrument: one, or none. Changing only a drum sampler's kit
    // keeps its device; in v1, setting a kit only names it (kits come in step 7).
    struct SetInstrument
    {
        std::string trackId;
        std::optional<Instrument> instrument;
    };

    // Replaces a drums track's drum map whole (SPEC §5.2). The notes don't
    // change, only the names its clips are written with.
    struct SetDrumMap
    {
        std::string trackId;
        std::vector<project::DrumMapEntry> entries; // a list, not a map, so a name given twice is reported
    };

    //==============================================================================
    // What a new clip holds: one of these.
    struct EmptyClip
    {
        int lengthInBars = 1;
    };

    struct ClipText
    {
        std::string notes; // .notes text, read with the track's drum map
    };

    // The same file as another clip: editing one edits both (SPEC §4.4).
    struct LinkedTo
    {
        std::string clipId;
    };

    // A file of its own, with a copy of another clip's notes (the GUI's duplicate).
    struct CopyOf
    {
        std::string clipId;
    };

    using ClipContent = std::variant<EmptyClip, ClipText, LinkedTo, CopyOf>;

    struct AddClip
    {
        std::string trackId;
        std::string start; // a song position, "9.1"
        ClipContent content;
        std::string id;   // optional: Hocket makes one from the track's, or a copy's or linked clip's from its clip's (SPEC §4.1)
        std::string file; // optional, "clips/verse-2.notes": named after the ID if not given; never for a linked clip
    };

    struct MoveClip
    {
        std::string clipId;
        std::string start;   // a song position, "9.1"
        std::string trackId; // optional: another track to move it to
    };

    // A file no other clip uses leaves the project.
    struct RemoveClip
    {
        std::string clipId;
    };

    // Gives a linked clip a file of its own, with a copy of its notes.
    struct MakeClipUnique
    {
        std::string clipId;
        std::string file; // optional: named after the clip's ID if not given
    };

    //==============================================================================
    // Transport commands (SPEC §3.1). They don't change the project, so they're
    // never validated against it, never enter the history, and don't split an
    // open transaction.
    struct Play
    {
    };

    struct Stop
    {
    };

    using Command = std::variant<WriteClip, SetTempo, SetTimeSignature, AddTrack, RemoveTrack, RenameTrack, MoveTrack, SetMix, SetInstrument,
                                 SetDrumMap, AddClip, MoveClip, RemoveClip, MakeClipUnique, Play, Stop>;

    // Who sends a command (SPEC §3.1). Two authors are the same when their names
    // and sessions match, so two Claude sessions are different authors, and a
    // client that reconnects with its session ID is still the author of its own
    // changes. The label only tells same-named authors apart in messages.
    struct Author
    {
        std::string name;    // "Lee", "Claude", "script:walking-bass"
        std::string session; // the client's session ID; empty for the person at the DAW
        std::string label;   // optional, chosen by the client, e.g. "drums"

        bool operator== (const Author& other) const { return name == other.name && session == other.session; }
    };

    // "Claude", or "Claude (drums)" with a label.
    std::string displayName (const Author& author);

    // How a message to `reader` names `author`: "you" for the reader; otherwise
    // the display name, plus " in another session" when it alone would look
    // like the reader's own.
    std::string nameFor (const Author& author, const Author& reader);

    struct Result
    {
        bool ok = false;
        std::string message; // on success, what happened; on failure, what's wrong and how to fix it
    };
}
