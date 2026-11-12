#include "core/project/Project.h"
#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <cassert>
#include <set>

namespace hocket::project
{
namespace
{
    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }
}

const notes::Clip& notesOf (const Project& project, const Clip& clip)
{
    const auto found = project.notesFiles.find (clip.file);
    assert (found != project.notesFiles.end()); // every clip's file is there (Project::notesFiles)
    return found->second;
}

notes::Ticks clipEnd (const Project& project, const Clip& clip)
{
    return clip.start + notesOf (project, clip).lengthInBars * project.meter.ticksPerBar();
}

notes::Ticks songEnd (const Project& project)
{
    const auto barTicks = project.meter.ticksPerBar();
    notes::Ticks end = 0;

    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            end = std::max (end, clipEnd (project, clip));

    return std::max<notes::Ticks> (1, (end + barTicks - 1) / barTicks) * barTicks;
}

std::vector<const Clip*> clipsUsing (const Project& project, std::string_view file)
{
    std::vector<const Clip*> clips;

    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            if (clip.file == file)
                clips.push_back (&clip);

    return clips;
}

std::vector<const Track*> tracksUsing (const Project& project, std::string_view file)
{
    std::vector<const Track*> tracks;

    for (const auto& track : project.tracks)
        if (std::ranges::any_of (track.clips, [file] (const Clip& clip) { return clip.file == file; }))
            tracks.push_back (&track);

    return tracks;
}

const notes::DrumMap* drumMapOf (const Track& track)
{
    return track.type == TrackType::drums ? &track.drumMap : nullptr;
}

const notes::DrumMap* drumMapOfFile (const Project& project, std::string_view file)
{
    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            if (clip.file == file)
                return drumMapOf (track);

    return nullptr;
}

std::string drumNameConflicts (const notes::Clip& content, const Track& a, const Track& b)
{
    static const notes::DrumMap noMap;
    const auto& mapA = drumMapOf (a) != nullptr ? *drumMapOf (a) : noMap;
    const auto& mapB = drumMapOf (b) != nullptr ? *drumMapOf (b) : noMap;

    std::set<int> pitches;

    for (const auto& note : content.notes)
        pitches.insert (note.pitch);

    std::string conflicts;

    for (const auto pitch : pitches)
    {
        const auto* nameA = mapA.nameOf (pitch);
        const auto* nameB = mapB.nameOf (pitch);

        if (nameA == nullptr && nameB == nullptr)
            continue;

        if (nameA != nullptr && nameB != nullptr && *nameA == *nameB)
            continue;

        auto conflict = "note " + std::to_string (pitch) + " (" + notes::formatPitch (pitch, notes::namedSpelling (pitch)) + ") is ";

        if (nameA != nullptr && nameB != nullptr)
        {
            conflict += singleQuoted (*nameA) + " on " + a.id + " but " + singleQuoted (*nameB) + " on " + b.id;
        }
        else
        {
            const auto& named = nameA != nullptr ? a : b;
            const auto& unnamed = nameA != nullptr ? b : a;
            const auto& name = nameA != nullptr ? *nameA : *nameB;

            conflict += singleQuoted (name) + " on " + named.id + " but ";

            if (drumMapOf (unnamed) == nullptr)
                conflict += unnamed.id + " is a \"" + trackTypeName (unnamed.type) + "\" track, with no drum map";
            else if (const auto elsewhere = unnamed.drumMap.noteOf (name))
                conflict += "has no name on " + unnamed.id + " (its " + singleQuoted (name) + " is note " + std::to_string (*elsewhere) + ")";
            else
                conflict += "has no name on " + unnamed.id;
        }

        conflicts += (conflicts.empty() ? "" : "; ") + conflict;
    }

    return conflicts;
}

const Clip* findClip (const Project& project, std::string_view id, const Track** track)
{
    for (const auto& candidateTrack : project.tracks)
    {
        for (const auto& clip : candidateTrack.clips)
        {
            if (clip.id == id)
            {
                if (track != nullptr)
                    *track = &candidateTrack;

                return &clip;
            }
        }
    }

    return nullptr;
}

const Track* findTrack (const Project& project, std::string_view id)
{
    const auto found = std::ranges::find (project.tracks, id, &Track::id);
    return found == project.tracks.end() ? nullptr : &*found;
}

std::optional<std::string> whatHasId (const Project& project, std::string_view id)
{
    for (const auto& track : project.tracks)
    {
        if (track.id == id)
            return "track " + singleQuoted (id) + " (\"" + track.name + "\")";

        for (const auto& device : track.devices)
            if (device.id == id)
                return "device " + singleQuoted (id) + " on " + track.id;

        for (const auto& clip : track.clips)
            if (clip.id == id)
                return "clip " + singleQuoted (id) + " on " + track.id;
    }

    return {};
}

std::string unknownTrackMessage (const Project& project, std::string_view id)
{
    if (findClip (project, id) != nullptr)
        return singleQuoted (id) + " is a clip ID; this needs a track ID like 'trk-bass'";

    std::string trackIds;

    for (const auto& track : project.tracks)
    {
        for (const auto& device : track.devices)
            if (device.id == id)
                return singleQuoted (id) + " is a device ID, on track " + singleQuoted (track.id) + "; this needs a track ID like 'trk-bass'";

        trackIds += (trackIds.empty() ? "" : ", ") + singleQuoted (track.id);
    }

    return "there's no track " + singleQuoted (id) + "; "
         + (trackIds.empty() ? std::string ("the project has no tracks yet") : "the tracks are " + trackIds);
}

std::string unknownClipMessage (const Project& project, std::string_view id, std::string_view toolName)
{
    std::string clipIds;

    for (const auto& track : project.tracks)
    {
        if (track.id == id)
            return singleQuoted (id) + " is a track ID; " + std::string (toolName) + " needs a clip ID like 'clp-bass'";

        for (const auto& device : track.devices)
            if (device.id == id)
                return singleQuoted (id) + " is a device ID; " + std::string (toolName) + " needs a clip ID like 'clp-bass'";

        for (const auto& clip : track.clips)
            clipIds += (clipIds.empty() ? "" : ", ") + singleQuoted (clip.id);
    }

    return "there's no clip " + singleQuoted (id) + "; "
         + (clipIds.empty() ? std::string ("the project has no clips yet") : "the clips are " + clipIds);
}

const char* trackTypeName (TrackType type)
{
    switch (type)
    {
        case TrackType::midi: return "midi";
        case TrackType::drums: return "drums";
    }

    return "midi";
}

const char* deviceTypeName (DeviceType type)
{
    switch (type)
    {
        case DeviceType::synth: return "hocket:synth";
        case DeviceType::drumSampler: return "hocket:drum-sampler";
    }

    return "hocket:synth";
}
}
