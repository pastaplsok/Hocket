#pragma once

// The command bus's edits: the smallest changes a command makes to the
// project, each with an exact inverse. A command is validated into a list of
// edits; its history entry keeps them and their inverses, so undo and redo
// replay them. Internal to the bus.

#include "core/commands/Changes.h"
#include "core/commands/Commands.h"
#include "core/project/Project.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace hocket::commands::edits
{
    // A clip file's notes. Without content, the file leaves the project, as
    // no clip uses it any more; a file the project doesn't have yet joins it.
    struct SetNotes
    {
        std::string file;
        std::optional<notes::Clip> content;

        bool operator== (const SetNotes&) const = default;
    };

    struct SetTempo
    {
        double bpm = 120.0;

        bool operator== (const SetTempo&) const = default;
    };

    // The time signature only: each file's @length changes with SetNotes.
    struct SetMeter
    {
        notes::Meter meter;

        bool operator== (const SetMeter&) const = default;
    };

    // A track with its clips, whose files must be in the project already.
    struct InsertTrack
    {
        std::size_t index = 0; // in the display order
        project::Track track;

        bool operator== (const InsertTrack&) const = default;
    };

    struct RemoveTrack
    {
        std::string trackId;

        bool operator== (const RemoveTrack&) const = default;
    };

    // To this place in the display order, counted without the track itself.
    struct MoveTrack
    {
        std::string trackId;
        std::size_t index = 0;

        bool operator== (const MoveTrack&) const = default;
    };

    // Everything about a track but its ID, type and clips: its name, mix,
    // devices and drum map. track.id says which track; its type and clips are ignored.
    struct SetTrack
    {
        project::Track track;

        bool operator== (const SetTrack&) const = default;
    };

    // Its file must be in the project already.
    struct InsertClip
    {
        std::string trackId;
        std::size_t index = 0; // in the track's clips
        project::Clip clip;

        bool operator== (const InsertClip&) const = default;
    };

    struct RemoveClip
    {
        std::string clipId;

        bool operator== (const RemoveClip&) const = default;
    };

    // A clip's start and file; clip.id says which clip. Its file must be in the project already.
    struct SetClip
    {
        project::Clip clip;

        bool operator== (const SetClip&) const = default;
    };

    using Edit = std::variant<SetNotes, SetTempo, SetMeter, InsertTrack, RemoveTrack, MoveTrack, SetTrack, InsertClip, RemoveClip, SetClip>;

    // Applies the edit and returns its inverse, adding what changed to
    // `changes`. The edit must fit the project: validation made it for
    // the project as it is.
    Edit apply (project::Project& project, const Edit& edit, std::vector<Change>& changes);

    // Clip files, by name ("clips/bass.notes").
    using Files = std::set<std::string, std::less<>>;

    // Adds the clip files whose text the edit could change: their notes, or
    // how they're written (the time signature, a drum map). Ask before
    // applying it and after, since some are only there before or after.
    void addFilesAffected (const project::Project& project, const Edit& edit, Files& files);

    // What a command will do, once validated.
    struct Plan
    {
        Result result;           // not ok: refused, saying why. Ok with no edits: it changes nothing, and says so
        std::vector<Edit> edits; // in the order they apply
        std::string description; // the history entry's: "added track trk-drums \"Drums\""
        std::string remarks;     // anything else the result says, a sentence after the entry's: "clp-b now starts off a bar line, at 2.2"
        std::string setting;     // for a gesture (SPEC §3.1): what it sets, e.g. "tempo"; changes to another setting end a gesture
    };
}
