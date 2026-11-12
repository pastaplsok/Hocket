#include "core/project/Project.h"

#include <algorithm>
#include <cassert>

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
    }

    return "midi";
}

const char* deviceTypeName (DeviceType type)
{
    switch (type)
    {
        case DeviceType::synth: return "hocket:synth";
    }

    return "hocket:synth";
}
}
