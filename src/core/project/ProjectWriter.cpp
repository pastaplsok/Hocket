#include "core/project/Project.h"
#include "core/notes/NotesSyntax.h"
#include "core/units/Units.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <tuple>

namespace hocket::project
{
namespace
{
    // ordered_json keeps keys in insertion order, which is the canonical order (SPEC §4.4).
    using Json = nlohmann::ordered_json;

    Json tempoValue (double tempo)
    {
        // Whole-number tempos are written without a decimal point: 120, not 120.0.
        if (std::floor (tempo) == tempo)
            return static_cast<std::int64_t> (tempo);

        return tempo;
    }

    Json clipValue (const Clip& clip, notes::Meter meter)
    {
        Json value = Json::object();
        value["id"] = clip.id;
        value["file"] = clip.file;
        value["start"] = notes::formatPosition (clip.start, meter);
        return value;
    }

    Json trackValue (const Track& track, notes::Meter meter)
    {
        Json value = Json::object();
        value["id"] = track.id;
        value["name"] = track.name;
        value["type"] = trackTypeName (track.type);
        value["volume"] = units::format (track.volume, units::volume());
        value["pan"] = units::formatPan (track.pan);
        value["mute"] = track.mute;
        value["solo"] = track.solo;

        auto& devices = value["devices"] = Json::array();

        for (const auto& device : track.devices)
        {
            Json deviceValue = Json::object();
            deviceValue["id"] = device.id;
            deviceValue["type"] = deviceTypeName (device.type);

            if (device.type == DeviceType::drumSampler)
                deviceValue["kit"] = device.kit;

            devices.push_back (std::move (deviceValue));
        }

        // A drums track's names for its notes, by note (SPEC §5.2).
        if (const auto* drumMap = drumMapOf (track))
        {
            auto& names = value["drumMap"] = Json::object();

            for (const auto& [note, name] : drumMap->byNote())
                names[name] = note;
        }

        // Clips sit on a timeline, so their canonical order is by start.
        std::vector<const Clip*> clips;

        for (const auto& clip : track.clips)
            clips.push_back (&clip);

        std::ranges::stable_sort (clips, [] (const Clip* a, const Clip* b)
        {
            return std::tie (a->start, a->id) < std::tie (b->start, b->id);
        });

        auto& clipValues = value["clips"] = Json::array();

        for (const auto* clip : clips)
            clipValues.push_back (clipValue (*clip, meter));

        return value;
    }
}

std::map<std::string, std::string> write (const Project& project)
{
    Json root = Json::object();
    root["format"] = "hocket-project";
    root["formatVersion"] = formatVersion;
    root["tempo"] = tempoValue (project.tempo);
    root["timeSignature"] = project.meter.text();

    auto& tracks = root["tracks"] = Json::array();

    for (const auto& track : project.tracks)
        tracks.push_back (trackValue (track, project.meter));

    std::map<std::string, std::string> files;
    files["project.json"] = root.dump (2) + "\n";

    // Once each, however many clips are linked to it.
    for (const auto& entry : project.notesFiles)
        files[entry.first] = *clipFileText (project, entry.first);

    return files;
}

std::optional<std::string> clipFileText (const Project& project, std::string_view file)
{
    const auto found = project.notesFiles.find (file);

    if (found == project.notesFiles.end())
        return {};

    return notes::write (found->second, project.meter, drumMapOfFile (project, file));
}
}
