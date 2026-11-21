#pragma once

// The transport (SPEC §3.1): play, stop, seek, the loop region, loop on and
// off. Its settings last the session: they aren't saved in the project, and
// changing them isn't undoable. The bus keeps them, validates the commands
// that change them, and says what each did; the engine carries them out.

#include "core/commands/Commands.h"
#include "core/notes/Notes.h"
#include "core/project/Project.h"

#include <optional>

namespace hocket::commands
{
    // A stretch of the song, from start up to but not including end.
    struct TickRange
    {
        notes::Ticks start = 0;
        notes::Ticks end = 0;

        notes::Ticks length() const { return end - start; }
        bool contains (notes::Ticks position) const { return position >= start && position < end; }
        bool operator== (const TickRange&) const = default;
    };

    struct TransportSettings
    {
        std::optional<TickRange> loopRegion; // none: play plays the whole song
        bool looping = true;                 // off: play runs to the end once, and stops
        notes::Ticks seekPosition = 0;       // the last seek's; play starts there when it's in what plays

        bool operator== (const TransportSettings&) const = default;
    };

    // What play plays: the loop region, or the whole song, from 1.1 to the end
    // of its last clip, rounded up to a bar (project::songEnd).
    TickRange playRange (const project::Project&, const TransportSettings&);

    // Where play starts: the last seek's position when it's in playRange,
    // otherwise playRange's start.
    notes::Ticks startPosition (const project::Project&, const TransportSettings&);

    // Where a playhead outside the range goes: the place it would have reached
    // if it had always looped the range, counted from the range's start. With
    // a range that's whole bars long, that's the same place in the bar
    // (SPEC §3.3). A position inside the range stays where it is.
    notes::Ticks wrapInto (notes::Ticks position, TickRange range);

    // How much has played since play last started (SPEC §6.4, the output line).
    struct PlayProgress
    {
        bool started = false;   // false: nothing has played this session
        double seconds = 0.0;   // from when play started to now, or to when it stopped
        notes::Ticks ticks = 0; // how far the playhead went in that time, every pass of a loop counted
    };

    // A move of the playhead that a change to the project made while it
    // played: a wrap into what plays, when the song came to end before it (SPEC §3.3).
    struct PlayheadWrap
    {
        notes::Ticks from = 0;
        notes::Ticks to = 0;
    };

    // Carries out transport commands for the bus, which keeps the settings
    // (CommandBus::transportSettings) and checks each command before it gets
    // here. The engine provides one; without it, transport commands fail.
    class Transport
    {
    public:
        virtual ~Transport() = default;

        // Since play last started.
        virtual PlayProgress playProgress() const { return {}; }

        // The wrap a change to the project or the transport made since this
        // was last asked, if any, and forgets it.
        virtual std::optional<PlayheadWrap> takeWrap() { return {}; }

        // Plays playRange from startPosition, on a loop or once, as the bus's
        // settings say; never called while playing.
        virtual void play() = 0;
        virtual void stop() = 0;
        virtual bool isPlaying() const = 0;

        // While playing: where the playhead is in the song.
        virtual notes::Ticks playheadPosition() const = 0;

        // While playing: jumps there. It's in playRange.
        virtual void jumpTo (notes::Ticks position) = 0;

        // The loop region, or looping, changed in the bus's settings. While
        // playing, a playhead outside the new playRange wraps into it (wrapInto).
        virtual void loopChanged() = 0;
    };

    // The level of what's played: --play's master meter (SPEC §9). Its peak and
    // count restart each time a transport play starts (SPEC §6.4).
    struct OutputLevel
    {
        float peak = 0.0f;                // the loudest sample since play started; 1.0 is full scale
        std::uint64_t clippedSamples = 0; // samples over full scale since then, which the driver cuts off
    };

    class OutputMeter
    {
    public:
        virtual ~OutputMeter() = default;
        virtual OutputLevel levelSincePlay() const = 0;
        virtual void restart() = 0;
    };

    // For the bus: checks a transport command against the project and the
    // settings, changes the settings and has the transport carry it out, and
    // says what it did and what the setting was before. Fails, changing
    // nothing, when there's no transport.
    Result runTransportCommand (const Command&, const project::Project&, TransportSettings&, Transport*);
}
