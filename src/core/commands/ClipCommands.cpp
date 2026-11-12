// Planning the clip commands.

#include "core/commands/Planning.h"
#include "core/notes/NotesSyntax.h"

namespace hocket::commands::planning
{
namespace
{
    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    std::string barsText (int bars)
    {
        return std::to_string (bars) + (bars == 1 ? "bar" : "bars");
    }

    std::string countText (std::size_t count, std::string_view noun)
    {
        return std::to_string (count) + " " + std::string (noun) + (count == 1 ? "" : "s");
    }

    edits::Plan refused (std::string message)
    {
        return { { false, std::move (message) }, {}, {}, {} };
    }
}

edits::Plan plan (const project::Project& project, const WriteClip& command)
{
    const project::Track* clipTrack = nullptr;
    const auto* clip = project::findClip (project, command.clipId, &clipTrack);

    if (clip == nullptr)
        return refused ("write_clip: " + project::unknownClipMessage (project, command.clipId, "write_clip"));

    // A drums track's clips are read with its drum map (SPEC §5.2).
    const auto* drumMap = project::drumMapOf (*clipTrack);
    auto parsed = notes::parse (command.notes, project.meter, drumMap);

    if (! parsed.clip)
    {
        auto message = "write_clip: the new text for clip " + singleQuoted (clip->id) + " has " + countText (parsed.errors.size(), "error")
                     + ", so nothing was changed:";

        for (const auto& error : parsed.errors)
            message += "\n" + notes::formatDiagnostic (error, clip->file);

        return refused (std::move (message));
    }

    // A longer @length mustn't run any clip that uses the file into the next
    // clip on its track, or past the end of the longest song. The tightest
    // limit is the one reported, so the length it suggests fits every clip.
    const auto meter = project.meter;
    const auto newLength = parsed.clip->lengthInBars * meter.ticksPerBar();
    const auto songLimit = project::maxSongLengthInBars * meter.ticksPerBar();
    const project::Clip* tightest = nullptr;
    const project::Clip* tightestNext = nullptr; // the clip after it, if that's what limits it
    notes::Ticks tightestRoom = 0;
    std::vector<std::string> linked;

    for (const auto& track : project.tracks)
    {
        for (const auto& user : track.clips)
        {
            if (user.file != clip->file)
                continue;

            if (&user != clip)
                linked.push_back (user.id);

            const project::Clip* next = nullptr;

            for (const auto& other : track.clips)
                if (&other != &user && other.start >= user.start && (next == nullptr || other.start < next->start))
                    next = &other;

            const auto room = next != nullptr && next->start < songLimit ? next->start - user.start : songLimit - user.start;

            if (tightest == nullptr || room < tightestRoom)
            {
                tightest = &user;
                tightestNext = next != nullptr && next->start < songLimit ? next : nullptr;
                tightestRoom = room;
            }
        }
    }

    if (newLength > tightestRoom)
    {
        const auto newEnd = tightest->start + newLength;
        const auto maxBars = static_cast<int> (tightestRoom / meter.ticksPerBar());
        const auto who = tightest == clip ? "clip " + singleQuoted (clip->id)
                                          : "clip " + singleQuoted (tightest->id) + " (linked to " + singleQuoted (clip->id) + ": they share "
                                                + clip->file + ")";

        const auto where = tightestNext != nullptr
                             ? ", into clip " + singleQuoted (tightestNext->id) + " at " + notes::formatPosition (tightestNext->start, meter)
                                   + ". Clips on a track can't overlap"
                             : ", past the end of bar " + std::to_string (project::maxSongLengthInBars) + ". A song is at most "
                                   + std::to_string (project::maxSongLengthInBars) + " bars long";

        return refused ("write_clip: with @length " + barsText (parsed.clip->lengthInBars) + ", " + who + " would run from "
                        + notes::formatPosition (tightest->start, meter) + " to " + notes::formatPosition (newEnd, meter) + where
                        + ", so make @length at most " + barsText (maxBars) + ". Nothing was changed");
    }

    // A file used on several tracks must mean the same notes on each, so their
    // drum maps must name the new notes alike (SPEC §5.2).
    for (const auto* track : project::tracksUsing (project, clip->file))
    {
        if (track == clipTrack)
            continue;

        if (const auto conflicts = project::drumNameConflicts (*parsed.clip, *clipTrack, *track); ! conflicts.empty())
            return refused ("write_clip: " + clip->file + " is used on tracks " + clipTrack->id + " and " + track->id
                            + ", and their drum maps don't name these notes alike: " + conflicts + ". A file used on several tracks "
                              "must mean the same notes on each, so nothing was changed. Leave those notes out, or make the maps agree");
    }

    // Compared in canonical form: the parser keeps notes and comments where the
    // text had them, so two texts that write out the same can parse differently,
    // e.g. a client sending back read_clip's text for a clip loaded from looser text.
    if (notes::write (*parsed.clip, meter, drumMap) == notes::write (project::notesOf (project, *clip), meter, drumMap))
        return { { true, "clip " + singleQuoted (clip->id) + " already has exactly these notes, so nothing changed and no history entry was added" },
                 {}, {}, {} };

    auto description = "wrote clip " + clip->id + " (" + countText (parsed.clip->notes.size(), "note") + ")";

    if (! linked.empty())
    {
        description += ", and its linked clip" + std::string (linked.size() == 1 ? " " : "s ");

        for (std::size_t i = 0; i < linked.size(); ++i)
            description += (i == 0 ? "" : ", ") + linked[i];
    }

    edits::Plan result { { true, {} }, {}, std::move (description), {} };
    result.edits.push_back (edits::SetNotes { clip->file, std::move (*parsed.clip) });
    return result;
}
}
