// Planning the track commands.

#include "core/commands/Planning.h"
#include "core/project/Rules.h"
#include "core/units/Units.h"

#include <algorithm>
#include <set>

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

//==============================================================================
edits::Plan plan (const project::Project& project, const SetInstrument& command)
{
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return refused ("set a track's instrument: " + project::unknownTrackMessage (project, command.trackId));

    const auto* current = track->devices.empty() ? nullptr : &track->devices.front();
    auto settings = settingsOf (*track);

    if (! command.instrument)
    {
        if (current == nullptr)
            return unchanged (track->id + " has no instrument already, so nothing changed and no history entry was added");

        settings.devices.clear();
        return { { true, {} }, { edits::SetTrack { std::move (settings) } }, "removed " + track->id + "'s instrument " + current->id, {} };
    }

    auto device = deviceFor (project, *command.instrument, *track,
                             "A track's type doesn't change, so add a \"drums\" track for a kit, or use \"hocket:synth\"");

    if (! device.device)
        return refused ("set " + track->id + "'s instrument: " + device.error);

    // The same kind of device: only its kit can differ, and it keeps its ID.
    if (current != nullptr && current->type == device.device->type)
    {
        if (current->kit == device.device->kit)
            return unchanged (track->id + "'s instrument is already " + describeDevice (*current) + ", so nothing changed and no history entry was added");

        settings.devices.front().kit = device.device->kit;
        return { { true, {} }, { edits::SetTrack { std::move (settings) } },
                 "set " + track->id + "'s kit to " + singleQuoted (device.device->kit) + ", on " + current->id, {} };
    }

    auto description = "set " + track->id + "'s instrument to " + describeDevice (*device.device);

    if (current != nullptr)
        description += ", replacing " + current->id;

    settings.devices = { std::move (*device.device) };
    return { { true, {} }, { edits::SetTrack { std::move (settings) } }, std::move (description), {} };
}

namespace
{
    // What changed between two drum maps, for the history: "renamed snare2 to
    // rim", "moved clap from 39 to 40", "added perc-1 (82)", "removed cowbell (56)".
    std::vector<std::string> drumMapChanges (const notes::DrumMap& before, const notes::DrumMap& after)
    {
        std::vector<std::string> renamed;
        std::vector<std::string> moved;
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::set<std::string, std::less<>> renamedTo;

        for (const auto& [note, name] : before.byNote())
        {
            if (after.noteOf (name))
                continue;

            if (const auto* now = after.nameOf (note); now != nullptr && ! before.noteOf (*now))
            {
                renamed.push_back ("renamed " + name + " to " + *now);
                renamedTo.insert (*now);
            }
            else
            {
                removed.push_back ("removed " + name + " (" + std::to_string (note) + ")");
            }
        }

        for (const auto& [note, name] : after.byNote())
        {
            if (const auto was = before.noteOf (name))
            {
                if (*was != note)
                    moved.push_back ("moved " + name + " from " + std::to_string (*was) + " to " + std::to_string (note));
            }
            else if (! renamedTo.contains (name))
            {
                added.push_back ("added " + name + " (" + std::to_string (note) + ")");
            }
        }

        std::vector<std::string> changes;

        for (auto* list : { &renamed, &moved, &added, &removed })
            changes.insert (changes.end(), list->begin(), list->end());

        return changes;
    }
}

edits::Plan plan (const project::Project& project, const SetDrumMap& command)
{
    const auto* track = project::findTrack (project, command.trackId);

    if (track == nullptr)
        return refused ("set a track's drum map: " + project::unknownTrackMessage (project, command.trackId));

    const auto fail = [track] (const std::string& why) { return refused ("set " + track->id + "'s drum map: " + why); };

    if (track->type != project::TrackType::drums)
        return fail ("only \"drums\" tracks have a drum map; " + track->id + " is a \"" + project::trackTypeName (track->type)
                     + "\" track, whose clips use note names, and a track's type doesn't change. To use drum names, add a \"drums\" track "
                       "and move the clips to it");

    auto settings = settingsOf (*track);
    settings.drumMap = {};

    if (const auto problems = project::buildDrumMap (command.entries, settings.drumMap); ! problems.empty())
    {
        auto message = countText (problems.size(), "problem") + ", so nothing was changed: ";

        for (std::size_t i = 0; i < problems.size(); ++i)
            message += (i == 0 ? "" : "; ") + problems[i].message;

        return fail (message);
    }

    if (settings.drumMap == track->drumMap)
        return unchanged (track->id + "'s drum map is already exactly that, so nothing changed and no history entry was added");

    // Files linked into other tracks must still mean the same notes on each (SPEC §5.2),
    // and the clips whose files are written differently now are worth naming.
    edits::Files files;
    std::vector<std::string> rewritten;

    for (const auto& clip : track->clips)
    {
        if (! files.insert (clip.file).second)
            continue;

        const auto& content = project.notesFiles.at (clip.file);

        for (const auto* other : project::tracksUsing (project, clip.file))
            if (other != track)
                if (const auto conflicts = project::drumNameConflicts (content, settings, *other); ! conflicts.empty())
                    return fail (clip.file + " is also used on " + other->id + ", and with this map the two tracks wouldn't name its notes alike: "
                                 + conflicts + ". A file used on several tracks must mean the same notes on each, so change both maps alike, "
                                   "or make the clips unique first");

        if (notes::write (content, project.meter, &track->drumMap) != notes::write (content, project.meter, &settings.drumMap))
            for (const auto* user : project::clipsUsing (project, clip.file))
                rewritten.push_back (user->id);
    }

    auto changes = drumMapChanges (track->drumMap, settings.drumMap);
    constexpr std::size_t shown = 4;

    if (changes.size() > shown + 1)
    {
        const auto more = changes.size() - shown;
        changes.resize (shown);
        changes.push_back (std::to_string (more) + " more changes");
    }

    std::string remarks;

    if (! rewritten.empty())
        remarks = (rewritten.size() == 1 ? rewritten.front() + " is" : "These clips are") + std::string (" written with the new names now")
                + (rewritten.size() == 1 ? "" : ": " + listText (rewritten));

    return { { true, {} }, { edits::SetTrack { std::move (settings) } }, "changed " + track->id + "'s drum map: " + listText (changes),
             std::move (remarks) };
}
}
