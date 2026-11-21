// Planning the replacement of the whole project (ReplaceProject), which a
// revert to a kept save uses too (CommandBus reads the save).
//
// It's carried out as the smallest edits that get from the project to the
// new one, the same edits other commands make, so undo takes it back exactly,
// and the engine changes only what changed: a track it doesn't touch plays on
// as it was (SPEC §3.3).

#include "core/commands/Planning.h"
#include "core/project/Rules.h"

#include <algorithm>
#include <set>

namespace hocket::commands::planning
{
namespace
{
    bool sameSettings (const project::Track& a, const project::Track& b)
    {
        return a.name == b.name && a.volume == b.volume && a.pan == b.pan && a.mute == b.mute && a.solo == b.solo && a.devices == b.devices
            && a.drumMap == b.drumMap;
    }

    // The track in `in` that `track` stays as: the same ID and type. A track
    // whose type changes goes and is added again, since a type can't be set.
    const project::Track* staying (const project::Project& in, const project::Track& track)
    {
        const auto* found = project::findTrack (in, track.id);
        return found != nullptr && found->type == track.type ? found : nullptr;
    }

    bool hasClip (const project::Track& track, std::string_view id)
    {
        return std::ranges::find (track.clips, id, &project::Clip::id) != track.clips.end();
    }

    template <typename Items>
    auto at (Items& items, std::size_t index)
    {
        return items.begin() + static_cast<std::ptrdiff_t> (index);
    }

    // Why a project couldn't be loaded, or nullopt if it could: written out,
    // it must read back with no errors, and hold no file that no clip uses.
    std::optional<std::string> validityProblem (const project::Project& candidate)
    {
        std::set<std::string, std::less<>> used;

        for (const auto& track : candidate.tracks)
            for (const auto& clip : track.clips)
                used.insert (clip.file);

        for (const auto& entry : candidate.notesFiles)
            if (! used.contains (entry.first))
                return "it has notes for " + entry.first + ", which no clip uses";

        const auto files = project::write (candidate);
        const auto read = project::parse (files.at ("project.json"), [&files] (const std::string& path) -> std::optional<std::string>
        {
            const auto found = files.find (path);
            return found == files.end() ? std::nullopt : std::optional<std::string> (found->second);
        });

        if (read.errors.empty())
            return {};

        auto problem = project::formatDiagnostic (read.errors.front());

        if (read.errors.size() > 1)
            problem += " (and " + countText (read.errors.size() - 1, "more problem") + ")";

        return problem;
    }
}

std::vector<edits::Edit> replacementEdits (const project::Project& from, const project::Project& to)
{
    std::vector<edits::Edit> edits;

    // Every file a clip will name is there before anything names it.
    for (const auto& [file, content] : to.notesFiles)
        if (const auto found = from.notesFiles.find (file); found == from.notesFiles.end() || ! (found->second == content))
            edits.push_back (edits::SetNotes { file, content });

    if (from.meter != to.meter)
        edits.push_back (edits::SetMeter { to.meter });

    if (from.tempo != to.tempo)
        edits.push_back (edits::SetTempo { to.tempo });

    // Tracks that go, with their clips.
    for (const auto& track : from.tracks)
        if (staying (to, track) == nullptr)
            edits.push_back (edits::RemoveTrack { track.id });

    // Clips that leave a track that stays: gone, or moving to another track.
    for (const auto& track : from.tracks)
        if (const auto* kept = staying (to, track))
            for (const auto& clip : track.clips)
                if (! hasClip (*kept, clip.id))
                    edits.push_back (edits::RemoveClip { clip.id });

    // The display order, front to back: a new track comes with its clips; a
    // track that stays moves into its place if it isn't there.
    std::vector<std::string> order;

    for (const auto& track : from.tracks)
        if (staying (to, track) != nullptr)
            order.push_back (track.id);

    for (std::size_t i = 0; i < to.tracks.size(); ++i)
    {
        const auto& track = to.tracks[i];
        const auto found = std::ranges::find (order, track.id);

        if (found == order.end())
        {
            edits.push_back (edits::InsertTrack { i, track });
            order.insert (at (order, i), track.id);
        }
        else if (found != at (order, i))
        {
            edits.push_back (edits::MoveTrack { track.id, i });
            order.erase (found);
            order.insert (at (order, i), track.id);
        }
    }

    // Each track that stays: its settings, then its clips, front to back.
    for (const auto& track : to.tracks)
    {
        const auto* was = staying (from, track);

        if (was == nullptr)
            continue; // added whole, above

        if (! sameSettings (*was, track))
        {
            auto settings = track;
            settings.clips.clear(); // SetTrack ignores them
            edits.push_back (edits::SetTrack { std::move (settings) });
        }

        // Its clips as the edits so far leave them.
        std::vector<std::string> clips;

        for (const auto& clip : was->clips)
            if (hasClip (track, clip.id))
                clips.push_back (clip.id);

        for (std::size_t i = 0; i < track.clips.size(); ++i)
        {
            const auto& clip = track.clips[i];
            const auto found = std::ranges::find (clips, clip.id);

            if (found != clips.end() && found == at (clips, i))
            {
                if (! (*std::ranges::find (was->clips, clip.id, &project::Clip::id) == clip))
                    edits.push_back (edits::SetClip { clip });

                continue;
            }

            // New on this track, or out of order: put in its place. A clip
            // removed and inserted by one command keeps playing as it was,
            // unless its start changed.
            if (found != clips.end())
            {
                edits.push_back (edits::RemoveClip { clip.id });
                clips.erase (found);
            }

            edits.push_back (edits::InsertClip { track.id, i, clip });
            clips.insert (at (clips, i), clip.id);
        }
    }

    // Files no clip uses now leave once nothing names them.
    for (const auto& entry : from.notesFiles)
        if (! to.notesFiles.contains (entry.first))
            edits.push_back (edits::SetNotes { entry.first, {} });

    return edits;
}

edits::Plan plan (const project::Project& project, const ReplaceProject& command)
{
    if (const auto problem = validityProblem (command.project))
        return refused ("replace the project: the new project isn't valid: " + *problem);

    // Compared as written: the same project, whatever order its clips are kept in.
    if (project::write (command.project) == project::write (project))
        return unchanged ("the project is already the same as the new one, so nothing changed and no history entry was added");

    return { { true, {} },
             replacementEdits (project, command.project),
             command.description.empty() ? std::string ("replaced the project") : command.description,
             replacementChanges (project, command.project),
             {} };
}
}

namespace hocket::commands
{
std::string replacementChanges (const project::Project& from, const project::Project& to)
{
    using planning::countText;
    using planning::listText;

    std::vector<std::string> parts;

    if (from.tempo != to.tempo)
        parts.push_back ("the tempo (" + project::formatTempo (from.tempo) + " BPM to " + project::formatTempo (to.tempo) + " BPM)");

    if (from.meter != to.meter)
        parts.push_back ("the time signature (" + from.meter.text() + " to " + to.meter.text() + ")");

    std::vector<std::string> added, removed, changed;

    for (const auto& track : to.tracks)
    {
        const auto* was = project::findTrack (from, track.id);

        if (was == nullptr)
            added.push_back (track.id);
        else if (! (*was == track))
            changed.push_back (track.id);
    }

    for (const auto& track : from.tracks)
        if (project::findTrack (to, track.id) == nullptr)
            removed.push_back (track.id);

    if (! added.empty())
        parts.push_back ("added " + listText (added));

    if (! removed.empty())
        parts.push_back ("removed " + listText (removed));

    if (! changed.empty())
        parts.push_back ("changed " + listText (changed));

    std::size_t rewritten = 0;

    for (const auto& [file, content] : to.notesFiles)
        if (const auto found = from.notesFiles.find (file); found != from.notesFiles.end() && ! (found->second == content))
            ++rewritten;

    if (rewritten > 0)
        parts.push_back ("rewrote the notes of " + countText (rewritten, "clip file"));

    std::string text = "Changes: ";

    for (std::size_t i = 0; i < parts.size(); ++i)
        text += (i == 0 ? "" : "; ") + parts[i];

    // Only the display order changed.
    return parts.empty() ? std::string ("Changes: the order of the tracks") : text;
}
}
