// Planning the track commands.

#include "core/commands/Planning.h"
#include "core/project/Rules.h"
#include "core/units/Units.h"

#include <algorithm>

namespace hocket::commands::planning
{
namespace
{
    // Where a TrackPlace puts a track.
    struct ResolvedPlace
    {
        std::size_t index = 0; // in the display order, counted without the track being moved
        std::string error;     // when the place isn't valid
        std::string text;      // for the description: " before trk-bass", " after trk-bass", or nothing for the end
    };

    ResolvedPlace resolvePlace (const project::Project& project, const TrackPlace& place, std::string_view moving)
    {
        std::vector<std::string_view> order;

        for (const auto& track : project.tracks)
            if (track.id != moving)
                order.push_back (track.id);

        if (! place.before.empty() && ! place.after.empty())
            return { 0, "give one place, before or after a track, not both", {} };

        const auto before = ! place.before.empty();
        const auto& anchor = before ? place.before : place.after;

        if (anchor.empty())
            return { order.size(), {}, {} };

        if (anchor == moving)
            return { 0, "a track can't go " + std::string (before ? "before" : "after") + " itself", {} };

        const auto found = std::ranges::find (order, anchor);

        if (found == order.end())
            return { 0, project::unknownTrackMessage (project, anchor), {} };

        const auto index = static_cast<std::size_t> (found - order.begin()) + (before ? 0 : 1);
        return { index, {}, (before ? " before " : " after ") + anchor };
    }

    // dev-bass-synth, dev-drums-kit: the track's words and the device's role,
    // cutting the track's words short rather than the role to fit (SPEC §4.1).
    std::string deviceId (const project::Project& project, std::string_view trackId, project::DeviceType type)
    {
        const std::string role = type == project::DeviceType::drumSampler ? "kit" : "synth";
        const auto prefix = project::idPrefix (project::IdKind::device);
        const auto room = project::maxIdLength - prefix.size() - role.size() - 1 - 3; // and "-99", should it need a number
        auto words = std::string (trackId.substr (project::idPrefix (project::IdKind::track).size())).substr (0, room);

        while (! words.empty() && words.back() == '-')
            words.pop_back();

        return project::unusedId (project, project::IdKind::device, words + "-" + role);
    }

    // An instrument as a device on a track of this type, or why it can't be:
    // the fix for a drum sampler on a midi track depends on the command.
    struct DeviceOrError
    {
        std::optional<project::Device> device;
        std::string error;
    };

    DeviceOrError deviceFor (const project::Project& project, const Instrument& instrument, const project::Track& track,
                             std::string_view placementFix)
    {
        const auto type = project::parseDeviceType (instrument.type);

        if (! type.value)
            return { {}, type.error };

        if (const auto problem = project::devicePlacementProblem (*type.value, track.type))
            return { {}, *problem + ". " + std::string (placementFix) };

        if (*type.value == project::DeviceType::drumSampler)
        {
            if (instrument.kit.empty())
                return { {}, "a \"hocket:drum-sampler\" needs a kit, e.g. \"acoustic\"" };

            if (const auto problem = project::kitNameProblem (instrument.kit))
                return { {}, *problem };
        }
        else if (! instrument.kit.empty())
        {
            return { {}, "only a \"hocket:drum-sampler\" has a kit; leave the kit out for \"" + instrument.type + "\"" };
        }

        return { project::Device { deviceId (project, track.id, *type.value), *type.value,
                                   *type.value == project::DeviceType::drumSampler ? instrument.kit : std::string() },
                 {} };
    }

    // "dev-drums-kit, a hocket:drum-sampler playing kit 'acoustic'"
    std::string describeDevice (const project::Device& device)
    {
        auto text = device.id + ", a " + project::deviceTypeName (device.type);

        if (device.type == project::DeviceType::drumSampler)
            text += " playing kit " + singleQuoted (device.kit);

        return text;
    }

    // A track's settings, to change and send as a SetTrack: its clips aren't needed.
    project::Track settingsOf (const project::Track& track)
    {
        auto settings = track;
        settings.clips.clear();
        return settings;
    }
}

//==============================================================================
edits::Plan plan (const project::Project& project, const AddTrack& command)
{
    const auto fail = [] (const std::string& why) { return refused ("add a track: " + why); };
    const auto type = project::parseTrackType (command.type);

    if (! type.value)
        return fail (type.error);

    if (const auto problem = project::trackNameProblem (command.name))
        return fail (*problem);

    project::Track track;
    track.type = *type.value;
    track.name = command.name;

    if (command.id.empty())
    {
        auto words = project::hyphenatedWords (command.name);
        track.id = project::unusedId (project, project::IdKind::track, words.empty() ? project::trackTypeName (track.type) : words);
    }
    else
    {
        if (const auto problem = project::idProblem (command.id, project::IdKind::track))
            return fail (*problem);

        if (const auto owner = project::whatHasId (project, command.id))
            return fail ("ID " + singleQuoted (command.id) + " is already used by " + *owner + "; IDs are unique across the project, "
                         "so choose another, or leave the ID out and Hocket will make one from the name");

        track.id = command.id;
    }

    const auto place = resolvePlace (project, command.place, {});

    if (! place.error.empty())
        return fail (place.error);

    std::vector<std::string> remarks;

    if (command.instrument)
    {
        auto device = deviceFor (project, *command.instrument, track, "Add it as a \"drums\" track, or use \"hocket:synth\"");

        if (! device.device)
            return fail (device.error);

        remarks.push_back ("Its instrument is " + describeDevice (*device.device));
        track.devices.push_back (std::move (*device.device));
    }

    if (track.type == project::TrackType::drums)
    {
        track.drumMap = notes::standardDrumMap();
        remarks.push_back ("It has the standard drum map: " + std::to_string (track.drumMap.size())
                           + " names on General MIDI's notes, such as kick (36), snare (38) and hat-closed (42)");
    }

    std::string remarkText;

    for (const auto& remark : remarks)
        remarkText += (remarkText.empty() ? "" : ". ") + remark;

    auto description = "added track " + track.id + " " + doubleQuoted (track.name) + place.text;
    return { { true, {} }, { edits::InsertTrack { place.index, std::move (track) } }, std::move (description), std::move (remarkText) };
}

edits::Plan plan (const project::Project& project, const RemoveTrack& command)
{
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return refused ("remove a track: " + project::unknownTrackMessage (project, command.trackId));

    std::vector<edits::Edit> planned { edits::RemoveTrack { track->id } };
    edits::Files files;

    // A file only this track's clips use leaves the project with them (SPEC §4.4).
    for (const auto& clip : track->clips)
    {
        if (! files.insert (clip.file).second)
            continue;

        const auto onThisTrack = [track] (const project::Clip* user)
        {
            return std::ranges::any_of (track->clips, [user] (const project::Clip& own) { return own.id == user->id; });
        };

        if (std::ranges::all_of (project::clipsUsing (project, clip.file), onThisTrack))
            planned.push_back (edits::SetNotes { clip.file, {} });
    }

    auto description = "removed track " + track->id + " " + doubleQuoted (track->name);

    if (track->clips.size() == 1)
        description += " and its clip " + track->clips.front().id;
    else if (! track->clips.empty())
        description += " and its " + std::to_string (track->clips.size()) + " clips";

    return { { true, {} }, std::move (planned), std::move (description), {} };
}

edits::Plan plan (const project::Project& project, const RenameTrack& command)
{
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return refused ("rename a track: " + project::unknownTrackMessage (project, command.trackId));

    if (const auto problem = project::trackNameProblem (command.name))
        return refused ("rename " + track->id + ": " + *problem);

    if (command.name == track->name)
        return unchanged (track->id + " is already called " + doubleQuoted (track->name) + ", so nothing changed and no history entry was added");

    auto settings = settingsOf (*track);
    settings.name = command.name;

    return { { true, {} }, { edits::SetTrack { std::move (settings) } },
             "renamed track " + track->id + " " + doubleQuoted (track->name) + " to " + doubleQuoted (command.name), {} };
}

edits::Plan plan (const project::Project& project, const MoveTrack& command)
{
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return refused ("move a track: " + project::unknownTrackMessage (project, command.trackId));

    if (command.place.before.empty() && command.place.after.empty())
        return refused ("move " + track->id + ": say where, before or after another track; to move it last, put it after the last track");

    const auto place = resolvePlace (project, command.place, track->id);

    if (! place.error.empty())
        return refused ("move " + track->id + ": " + place.error);

    const auto current = static_cast<std::size_t> (track - project.tracks.data());

    if (place.index == current)
        return unchanged (track->id + " is already" + place.text + ", so nothing changed and no history entry was added");

    return { { true, {} }, { edits::MoveTrack { track->id, place.index } }, "moved track " + track->id + place.text, {} };
}

edits::Plan plan (const project::Project& project, const SetMix& command)
{
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return refused ("set a track's mix: " + project::unknownTrackMessage (project, command.trackId));

    const auto fail = [track] (const std::string& why) { return refused ("set " + track->id + "'s mix: " + why); };

    if (! command.volume && ! command.pan && ! command.mute && ! command.solo)
        return fail ("say what to set: any of its volume (e.g. \"-6dB\"), pan (e.g. \"L20\"), mute and solo");

    auto settings = settingsOf (*track);
    std::vector<std::string> parts;

    if (command.volume)
    {
        const auto volume = units::parse (*command.volume, units::volume());

        if (! volume.value)
            return fail (volume.error);

        if (*volume.value != track->volume)
            parts.push_back ("volume to " + units::format (*volume.value, units::volume()));

        settings.volume = *volume.value;
    }

    if (command.pan)
    {
        const auto pan = units::parsePan (*command.pan);

        if (! pan.value)
            return fail (pan.error);

        if (*pan.value != track->pan)
            parts.push_back ("pan to " + units::formatPan (*pan.value));

        settings.pan = *pan.value;
    }

    if (command.mute)
    {
        if (*command.mute != track->mute)
            parts.push_back (*command.mute ? "mute on" : "mute off");

        settings.mute = *command.mute;
    }

    if (command.solo)
    {
        if (*command.solo != track->solo)
            parts.push_back (*command.solo ? "solo on" : "solo off");

        settings.solo = *command.solo;
    }

    if (parts.empty())
        return unchanged (track->id + "'s mix already has those settings, so nothing changed and no history entry was added");

    std::string description;

    if (parts.size() == 1 && parts.front().starts_with ("mute "))
        description = (settings.mute ? "muted " : "unmuted ") + track->id;
    else if (parts.size() == 1 && parts.front().starts_with ("solo "))
        description = (settings.solo ? "soloed " : "unsoloed ") + track->id;
    else
        description = "set " + track->id + "'s " + listText (parts);

    return { { true, {} }, { edits::SetTrack { std::move (settings) } }, std::move (description), {} };
}
}
