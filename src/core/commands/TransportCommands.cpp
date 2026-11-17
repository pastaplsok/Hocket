// The transport commands (SPEC §3.1): checked against the project and the
// session's settings, and their results, which say what each setting was
// before, since they can't be undone.

#include "core/commands/Transport.h"
#include "core/notes/NotesSyntax.h"
#include "core/project/Rules.h"

#include <type_traits>

namespace hocket::commands
{
namespace
{
    // Whatever the command, for the message when there's no engine.
    std::string commandName (const Command& command)
    {
        return std::visit ([] (const auto& c) -> std::string
        {
            using Kind = std::decay_t<decltype (c)>;

            if constexpr (std::is_same_v<Kind, Play>)
                return "play";
            else if constexpr (std::is_same_v<Kind, Stop>)
                return "stop";
            else if constexpr (std::is_same_v<Kind, Seek>)
                return "seek";
            else if constexpr (std::is_same_v<Kind, SetLoopRegion>)
                return "set the loop region";
            else if constexpr (std::is_same_v<Kind, ClearLoopRegion>)
                return "clear the loop region";
            else if constexpr (std::is_same_v<Kind, SetLooping>)
                return c.on ? "turn loop on" : "turn loop off";
            else
                return {};
        }, command);
    }

    class TransportCommands
    {
    public:
        TransportCommands (const project::Project& p, TransportSettings& s, Transport& t) : project (p), settings (s), transport (t) {}

        Result run (const Play&)
        {
            if (transport.isPlaying())
                return { true, "already playing" };

            transport.play();

            const auto range = playRange (project, settings);
            const auto start = startPosition (project, settings);
            auto message = "playing " + std::string (settings.loopRegion ? "the loop region " : "") + text (range)
                         + (settings.looping ? " on a loop" : " once (loop is off)");

            if (start != range.start)
                message += ", starting at " + text (start);

            return { true, message };
        }

        Result run (const Stop&)
        {
            if (! transport.isPlaying())
                return { true, "already stopped" };

            transport.stop();
            return { true, "stopped" };
        }

        Result run (const Seek& command)
        {
            const auto parsed = notes::parsePosition (command.position, project.meter);

            if (! parsed.value)
                return { false, "seek: " + parsed.error };

            const auto target = *parsed.value;
            const auto songEnd = project::songEnd (project);
            const auto& region = settings.loopRegion;

            // Past the end of the song is most likely a typo, unless the loop
            // region runs on past it and the position's there.
            if (target >= songEnd && ! (region && region->contains (target)))
            {
                auto message = "seek: " + text (target) + " isn't in the song, which runs from " + text (0) + " up to " + text (songEnd)
                             + "; seek to a position before " + text (songEnd);

                if (region && region->end > songEnd)
                    message += ", or in the loop region " + text (*region);

                return { false, message };
            }

            const auto playing = transport.isPlaying();
            const auto from = playing ? playheadPosition() : startPosition (project, settings);

            if (! playing && from == target)
            {
                settings.seekPosition = target;
                return { true, "play already starts at " + text (target) };
            }

            settings.seekPosition = target;
            const auto range = playRange (project, settings);
            auto message = "seeked to " + text (target) + ", from " + text (from);

            if (playing)
            {
                const auto landed = wrapInto (target, range);
                transport.jumpTo (landed);

                if (landed != target)
                    message += "; that's outside the loop region " + text (range) + ", so the playhead wrapped into it, to " + text (landed);
            }
            else if (range.contains (target))
            {
                message += "; play starts there";
            }
            else
            {
                message += "; that's outside the loop region " + text (range) + ", so play starts at its start, " + text (range.start);
            }

            return { true, message };
        }

        Result run (const SetLoopRegion& command)
        {
            const auto fail = [] (const std::string& why) { return Result { false, "set the loop region: " + why }; };
            const auto start = notes::parsePosition (command.start, project.meter);

            if (! start.value)
                return fail ("its start, " + start.error);

            const auto end = notes::parsePosition (command.end, project.meter);

            if (! end.value)
                return fail ("its end, " + end.error);

            if (*end.value <= *start.value)
                return fail ("it would end at " + text (*end.value) + ", which isn't after its start, " + text (*start.value)
                             + "; the end comes after the start, e.g. 5.1 to 9.1");

            const auto songLimit = static_cast<notes::Ticks> (project::maxSongLengthInBars) * project.meter.ticksPerBar();

            if (*end.value > songLimit)
                return fail ("it would end at " + text (*end.value) + ", but a song is at most " + std::to_string (project::maxSongLengthInBars)
                             + " bars long, so it ends by " + text (songLimit));

            if (*end.value - *start.value < notes::ticksPerSixteenth)
                return fail (text (*start.value) + " to " + text (*end.value) + " is shorter than a sixteenth note, the shortest loop region");

            const TickRange region { *start.value, *end.value };
            const auto previous = settings.loopRegion;

            if (previous == region)
                return { true, "the loop region is already " + text (region) + ", so nothing changed" };

            const auto before = beforeChange();
            settings.loopRegion = region;
            transport.loopChanged();

            auto message = "loop region " + text (region) + bars (region) + ", was " + (previous ? text (*previous) : std::string ("the whole song"));
            const auto songEnd = project::songEnd (project);

            if (region.start >= songEnd)
                message += "; the song ends at " + text (songEnd) + ", before it starts, so it's silent until clips are added there";
            else if (region.end > songEnd)
                message += "; the song ends at " + text (songEnd) + ", inside it, so the rest is silent until clips are added there";

            return { true, message + afterChange (before) };
        }

        Result run (const ClearLoopRegion&)
        {
            const auto song = playRange (project, {});

            if (! settings.loopRegion)
                return { true, "there's no loop region, so nothing changed: play plays the whole song, " + text (song) };

            const auto previous = *settings.loopRegion;
            const auto before = beforeChange();
            settings.loopRegion.reset();
            transport.loopChanged();

            return { true, "loop region cleared, was " + text (previous) + ": play plays the whole song, " + text (song) + afterChange (before) };
        }

        Result run (const SetLooping& command)
        {
            if (settings.looping == command.on)
                return { true, std::string ("loop is already ") + (command.on ? "on" : "off") + ", so nothing changed" };

            const auto before = beforeChange();
            settings.looping = command.on;
            transport.loopChanged();

            const auto range = playRange (project, settings);
            const auto message = command.on ? "loop on, was off: play loops " + (settings.loopRegion ? "the loop region " : std::string ("the whole song, ")) + text (range)
                                            : "loop off, was on: play stops at the end of " + (settings.loopRegion ? "the loop region, " : std::string ("the song, "))
                                                  + text (range.end) + ", instead of looping";

            return { true, message + afterChange (before, false) };
        }

        template <typename Other>
        Result run (const Other&)
        {
            return { false, "not a transport command" }; // the bus only sends transport commands here
        }

    private:
        const project::Project& project;
        TransportSettings& settings;
        Transport& transport;

        std::string text (notes::Ticks position) const { return notes::formatPosition (position, project.meter); }
        std::string text (TickRange range) const { return text (range.start) + " to " + text (range.end); }

        // " (4 bars)", or nothing when it isn't whole bars.
        std::string bars (TickRange range) const
        {
            const auto barTicks = project.meter.ticksPerBar();

            if (range.length() % barTicks != 0)
                return {};

            const auto count = range.length() / barTicks;
            return " (" + std::to_string (count) + (count == 1 ? " bar)" : " bars)");
        }

        // While playing, where the playhead is, to the sixteenth below: it
        // has moved on by the time anyone reads it.
        notes::Ticks playheadPosition() const
        {
            const auto position = transport.playheadPosition();
            return position - position % notes::ticksPerSixteenth;
        }

        // Where the playhead is before a change to what plays; none when stopped.
        std::optional<notes::Ticks> beforeChange() const
        {
            if (! transport.isPlaying())
                return {};

            return transport.playheadPosition();
        }

        // What the change to what plays did to the playhead, or, when
        // stopped, where play now starts.
        std::string afterChange (std::optional<notes::Ticks> playhead, bool sayStart = true) const
        {
            const auto range = playRange (project, settings);
            std::string message;

            if (playhead && ! range.contains (*playhead))
            {
                const auto rounded = [] (notes::Ticks position) { return position - position % notes::ticksPerSixteenth; };
                message += "; the playhead was at " + text (rounded (*playhead)) + ", outside " + (settings.loopRegion ? "the loop region" : "the song")
                         + ", so it wrapped to " + text (rounded (wrapInto (*playhead, range)));
            }
            else if (! playhead && sayStart)
            {
                message += "; play starts at " + text (startPosition (project, settings));
            }

            return message;
        }
    };
}

//==============================================================================
bool isTransportCommand (const Command& command)
{
    return std::holds_alternative<Play> (command) || std::holds_alternative<Stop> (command) || std::holds_alternative<Seek> (command)
        || std::holds_alternative<SetLoopRegion> (command) || std::holds_alternative<ClearLoopRegion> (command)
        || std::holds_alternative<SetLooping> (command);
}

TickRange playRange (const project::Project& project, const TransportSettings& settings)
{
    return settings.loopRegion.value_or (TickRange { 0, project::songEnd (project) });
}

notes::Ticks startPosition (const project::Project& project, const TransportSettings& settings)
{
    const auto range = playRange (project, settings);
    return range.contains (settings.seekPosition) ? settings.seekPosition : range.start;
}

notes::Ticks wrapInto (notes::Ticks position, TickRange range)
{
    if (range.contains (position) || range.length() <= 0)
        return range.length() <= 0 ? range.start : position;

    // Counted from the range's start, before it too.
    const auto offset = (position - range.start) % range.length();
    return range.start + (offset < 0 ? offset + range.length() : offset);
}

Result runTransportCommand (const Command& command, const project::Project& project, TransportSettings& settings, Transport* transport)
{
    if (transport == nullptr)
    {
        const auto name = commandName (command);
        const auto play = std::holds_alternative<Play> (command) || std::holds_alternative<Stop> (command);
        return { false, name + ": there's no audio engine running, so " + (play ? "playback can't be started or stopped here" : "there's no transport here") };
    }

    TransportCommands commands (project, settings, *transport);
    return std::visit ([&commands] (const auto& c) { return commands.run (c); }, command);
}
}
