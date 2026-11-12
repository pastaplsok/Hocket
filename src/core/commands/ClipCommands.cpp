// Planning the clip commands.

#include "core/commands/Planning.h"
#include "core/notes/NotesSyntax.h"
#include "core/project/Rules.h"

#include <algorithm>
#include <tuple>

namespace hocket::commands::planning
{
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
        return unchanged ("clip " + singleQuoted (clip->id) + " already has exactly these notes, so nothing changed and no history entry was added");

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

//==============================================================================
namespace
{
    // A copy's or linked clip's ID, from its clip's (SPEC §4.1): clp-verse
    // gives clp-verse-2, and clp-verse-2 gives clp-verse-3.
    std::string relatedId (const project::Project& project, std::string_view sourceId)
    {
        const auto words = sourceId.substr (project::idPrefix (project::IdKind::clip).size());
        const auto hyphen = words.rfind ('-');

        if (hyphen != std::string_view::npos && hyphen > 0 && hyphen + 1 < words.size() && words.size() - hyphen - 1 <= 6
            && words[hyphen + 1] != '0' && std::ranges::all_of (words.substr (hyphen + 1), [] (char c) { return c >= '0' && c <= '9'; }))
            return project::unusedId (project, project::IdKind::clip, words.substr (0, hyphen), std::stoi (std::string (words.substr (hyphen + 1))) + 1);

        return project::unusedId (project, project::IdKind::clip, words, 2);
    }

    // Where a clip goes in its track's list: by start, as the writer orders them.
    std::size_t sortedIndex (const project::Track& track, const project::Clip& clip)
    {
        return static_cast<std::size_t> (std::ranges::count_if (track.clips, [&clip] (const project::Clip& other)
        {
            return std::tie (other.start, other.id) < std::tie (clip.start, clip.id);
        }));
    }

    // A file a new clip names: valid, and not another clip's.
    std::optional<std::string> newFileProblem (const project::Project& project, const std::string& file, std::string_view sharing)
    {
        if (auto problem = project::clipFileProblem (file))
            return problem;

        if (const auto users = project::clipsUsing (project, file); ! users.empty())
            return file + " is already used by clip " + users.front()->id + "; to share a file with it, " + std::string (sharing)
                 + ", or name another file";

        return {};
    }

    // Whether `track` would name the file's notes as the other tracks using it do (SPEC §5.2).
    std::optional<std::string> drumNamesProblem (const project::Project& project, const std::string& file, const project::Track& track,
                                                 const project::Track* leaving, std::string_view fix)
    {
        const auto& content = project.notesFiles.at (file);

        for (const auto* other : project::tracksUsing (project, file))
        {
            if (other == &track)
                continue;

            // A track whose only clip using the file is the one leaving it.
            if (other == leaving && std::ranges::count_if (other->clips, [&file] (const project::Clip& c) { return c.file == file; }) == 1)
                continue;

            if (const auto conflicts = project::drumNameConflicts (content, track, *other); ! conflicts.empty())
                return file + " is used on " + other->id + ", and the two tracks' drum maps don't name its notes alike: " + conflicts
                     + ". A file used on several tracks must mean the same notes on each, so make the maps agree, or " + std::string (fix);
        }

        return {};
    }
}

edits::Plan plan (const project::Project& project, const AddClip& command)
{
    const auto fail = [] (const std::string& why) { return refused ("add a clip: " + why); };
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return fail (project::unknownTrackMessage (project, command.trackId));

    const auto start = notes::parsePosition (command.start, project.meter);

    if (! start.value)
        return fail (start.error);

    // The clip a linked clip or a copy comes from.
    const auto* linkedTo = std::get_if<LinkedTo> (&command.content);
    const auto* copyOf = std::get_if<CopyOf> (&command.content);
    const project::Clip* source = nullptr;

    if (linkedTo != nullptr || copyOf != nullptr)
    {
        const auto& sourceId = linkedTo != nullptr ? linkedTo->clipId : copyOf->clipId;
        source = project::findClip (project, sourceId);

        if (source == nullptr)
            return fail (project::unknownClipMessage (project, sourceId, "this"));
    }

    std::string id;

    if (command.id.empty())
    {
        id = source != nullptr ? relatedId (project, source->id)
                               : project::unusedId (project, project::IdKind::clip, track->id.substr (project::idPrefix (project::IdKind::track).size()));
    }
    else
    {
        if (const auto problem = project::idProblem (command.id, project::IdKind::clip))
            return fail (*problem);

        if (const auto owner = project::whatHasId (project, command.id))
            return fail ("ID " + singleQuoted (command.id) + " is already used by " + *owner + "; IDs are unique across the project, "
                         "so choose another, or leave the ID out and Hocket will make one");

        id = command.id;
    }

    // Its file, and the notes for a new one.
    std::string file;
    std::optional<notes::Clip> content;
    std::string what;

    if (linkedTo != nullptr)
    {
        if (! command.file.empty())
            return fail ("a linked clip uses its clip's file, " + source->file + "; leave the file out, or add a copy to give it one of its own");

        file = source->file;
        what = ", linked to " + source->id;
    }
    else
    {
        file = command.file.empty() ? project::unusedClipFile (project, id) : command.file;

        if (const auto problem = newFileProblem (project, file, "add the clip linked to it"))
            return fail (*problem);

        if (const auto* empty = std::get_if<EmptyClip> (&command.content))
        {
            if (empty->lengthInBars < 1 || empty->lengthInBars > notes::maxLengthInBars)
                return fail ("an empty clip is 1 to " + std::to_string (notes::maxLengthInBars) + " bars long, not " + std::to_string (empty->lengthInBars));

            content.emplace();
            content->lengthInBars = empty->lengthInBars;
            what = " (empty, " + barsText (empty->lengthInBars) + ")";
        }
        else if (const auto* text = std::get_if<ClipText> (&command.content))
        {
            auto parsed = notes::parse (text->notes, project.meter, project::drumMapOf (*track));

            if (! parsed.clip)
            {
                auto message = "the clip's text has " + countText (parsed.errors.size(), "error") + ", so nothing was changed:";

                for (const auto& error : parsed.errors)
                    message += "\n" + notes::formatDiagnostic (error, file);

                return fail (message);
            }

            content = std::move (*parsed.clip);
            what = " (" + countText (content->notes.size(), "note") + ")";
        }
        else
        {
            content = project::notesOf (project, *source);
            what = ", a copy of " + source->id;
        }
    }

    const auto length = (content ? *content : project::notesOf (project, *source)).lengthInBars * project.meter.ticksPerBar();

    if (const auto problem = project::clipPlacementProblem (project, *track, *start.value, length))
        return fail (*problem);

    if (linkedTo != nullptr)
        if (const auto problem = drumNamesProblem (project, file, *track, nullptr, "add a copy instead"))
            return fail (*problem);

    const project::Clip clip { id, file, *start.value };
    std::vector<edits::Edit> planned;

    if (content)
        planned.push_back (edits::SetNotes { file, std::move (*content) });

    planned.push_back (edits::InsertClip { track->id, sortedIndex (*track, clip), clip });

    return { { true, {} }, std::move (planned), "added clip " + id + " on " + track->id + " at " + notes::formatPosition (clip.start, project.meter) + what,
             linkedTo != nullptr ? std::string() : "Its notes are in " + file };
}

edits::Plan plan (const project::Project& project, const MoveClip& command)
{
    const project::Track* from = nullptr;
    const auto* clip = project::findClip (project, command.clipId, &from);

    if (clip == nullptr)
        return refused ("move a clip: " + project::unknownClipMessage (project, command.clipId, "this"));

    const auto fail = [clip] (const std::string& why) { return refused ("move " + clip->id + ": " + why); };
    const auto start = notes::parsePosition (command.start, project.meter);

    if (! start.value)
        return fail (start.error);

    const auto* to = command.trackId.empty() ? from : project::findTrack (project, command.trackId);

    if (to == nullptr)
        return fail (project::unknownTrackMessage (project, command.trackId));

    const auto position = notes::formatPosition (*start.value, project.meter);

    if (to == from && *start.value == clip->start)
        return unchanged (clip->id + " is already at " + position + ", so nothing changed and no history entry was added");

    if (const auto problem = project::clipPlacementProblem (project, *to, *start.value, project::clipEnd (project, *clip) - clip->start, clip->id))
        return fail (*problem);

    auto moved = *clip;
    moved.start = *start.value;

    if (to == from)
        return { { true, {} }, { edits::SetClip { moved } }, "moved " + clip->id + " to " + position, {} };

    if (const auto problem = drumNamesProblem (project, clip->file, *to, from, "make the clip unique first"))
        return fail (*problem);

    std::string remarks;

    if (to->type != from->type)
        remarks = to->id + (to->type == project::TrackType::drums ? " is a \"drums\" track, so the notes its drum map names are written by name there"
                                                                  : " is a \"midi\" track, so its notes are written as note names there");

    return { { true, {} },
             { edits::RemoveClip { clip->id }, edits::InsertClip { to->id, sortedIndex (*to, moved), moved } },
             "moved " + clip->id + " to " + position + " on " + to->id,
             std::move (remarks) };
}

edits::Plan plan (const project::Project& project, const RemoveClip& command)
{
    const project::Track* track = nullptr;
    const auto* clip = project::findClip (project, command.clipId, &track);

    if (clip == nullptr)
        return refused ("remove a clip: " + project::unknownClipMessage (project, command.clipId, "this"));

    std::vector<edits::Edit> planned { edits::RemoveClip { clip->id } };

    // A file no clip uses any more leaves the project (SPEC §4.4).
    if (project::clipsUsing (project, clip->file).size() == 1)
        planned.push_back (edits::SetNotes { clip->file, {} });

    return { { true, {} }, std::move (planned), "removed clip " + clip->id + " from " + track->id, {} };
}

edits::Plan plan (const project::Project& project, const MakeClipUnique& command)
{
    const auto* clip = project::findClip (project, command.clipId);

    if (clip == nullptr)
        return refused ("make a clip unique: " + project::unknownClipMessage (project, command.clipId, "this"));

    if (project::clipsUsing (project, clip->file).size() == 1)
        return unchanged (clip->id + " isn't linked to another clip: it has " + clip->file + " to itself, so nothing changed and no history entry was added");

    const auto file = command.file.empty() ? project::unusedClipFile (project, clip->id) : command.file;

    if (const auto problem = newFileProblem (project, file, "leave the clip linked"))
        return refused ("make " + clip->id + " unique: " + *problem);

    auto unique = *clip;
    unique.file = file;

    return { { true, {} },
             { edits::SetNotes { file, project::notesOf (project, *clip) }, edits::SetClip { std::move (unique) } },
             "made clip " + clip->id + " unique, with its own file " + file,
             {} };
}
}
