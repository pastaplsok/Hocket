#include "core/project/Project.h"

#include <algorithm>

namespace hocket::project
{
namespace
{
    std::string singleQuoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }
}

notes::Ticks clipEnd (const Clip& clip, notes::Meter meter)
{
    return clip.start + clip.content.lengthInBars * meter.ticksPerBar();
}

notes::Ticks songEnd (const Project& project)
{
    const auto barTicks = project.meter.ticksPerBar();
    notes::Ticks end = 0;

    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            end = std::max (end, clipEnd (clip, project.meter));

    return std::max<notes::Ticks> (1, (end + barTicks - 1) / barTicks) * barTicks;
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
        case DeviceType::fourOsc: return "tracktion:4osc";
    }

    return "tracktion:4osc";
}
}
