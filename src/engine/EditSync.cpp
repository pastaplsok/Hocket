#include "engine/EditSync.h"

#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <type_traits>
#include <stdexcept>

namespace hocket::engine
{
namespace
{
    juce::String juceString (const std::string& utf8)
    {
        return juce::String::fromUTF8 (utf8.data(), static_cast<int> (utf8.size()));
    }

    // Hocket counts ticks, 960 per quarter note; Tracktion counts beats, which
    // are the time signature's (eighths in 6/8), since Hocket keeps Tracktion's
    // default EngineBehaviour::lengthOfOneBeatDependsOnTimeSignature(). Its BPM
    // is then quarter notes per minute, as Hocket's is (SPEC §4.4).
    tracktion::BeatPosition beatPosition (notes::Ticks ticks, notes::Meter meter)
    {
        return tracktion::BeatPosition::fromBeats (static_cast<double> (ticks) / static_cast<double> (meter.ticksPerBeat()));
    }

    tracktion::BeatDuration beatDuration (notes::Ticks ticks, notes::Meter meter)
    {
        return tracktion::BeatDuration::fromBeats (static_cast<double> (ticks) / static_cast<double> (meter.ticksPerBeat()));
    }

    tracktion::Plugin::Ptr createDevice (tracktion::Edit& edit, const project::Device& device)
    {
        switch (device.type)
        {
            // 4OSC stands in for Hocket's synth until Milestone 4 (SPEC §8.1).
            case project::DeviceType::synth:
                return edit.getPluginCache().createNewPlugin (tracktion::FourOscPlugin::xmlTypeName, {});

            // Hocket's drum sampler is Milestone 1's step 7; until then its track is silent.
            case project::DeviceType::drumSampler:
                return {};
        }

        return {};
    }

    // Makes the Tracktion clip match the model's: position, length and notes.
    void writeClip (tracktion::MidiClip& target, const project::Project& project, const project::Clip& clip)
    {
        const auto meter = project.meter;
        const tracktion::BeatRange beats { beatPosition (clip.start, meter), beatPosition (hocket::project::clipEnd (project, clip), meter) };
        target.setPosition ({ target.edit.tempoSequence.toTime (beats) });

        // Undo is Hocket's job (SPEC §3.3), so no UndoManager. Notes that run past
        // the clip end are cut off there by Tracktion, as SPEC §5.4 says.
        auto& sequence = target.getSequence();
        sequence.clear (nullptr);

        for (const auto& note : hocket::project::notesOf (project, clip).notes)
            sequence.addNote (note.pitch, beatPosition (note.start, meter), beatDuration (note.length, meter), note.velocity, 0, nullptr);
    }
}

//==============================================================================
EditSync::EditSync (tracktion::Engine& engine, commands::CommandBus& commandBus) : bus (commandBus)
{
    const auto& project = bus.project();
    auto state = tracktion::createEmptyEdit (engine);

    tracktionEdit = tracktion::Edit::createEdit ({
        .engine = engine,
        .editState = state,
        .editProjectItemID = tracktion::ProjectItemID::fromProperty (state, tracktion::IDs::projectID),
        .role = tracktion::Edit::forEditing,
        .numUndoLevelsToStore = 0,
        .numAudioTracks = 0, // made here, by ID
    });

    if (tracktionEdit == nullptr)
        throw std::runtime_error ("Tracktion couldn't create an Edit");

    auto& tempo = tracktionEdit->tempoSequence;
    tempo.getTempo (0)->setBpm (project.tempo);
    tempo.getTimeSig (0)->setStringTimeSig (juceString (project.meter.text()));

    for (const auto& track : project.tracks)
        syncTrack (track.id);

    clearTracktionUndo();
    listenerId = bus.addListener ([this] (const commands::Change& change) { changed (change); });
    bus.setTransport (this);
}

EditSync::~EditSync()
{
    bus.setTransport (nullptr);
    bus.removeListener (listenerId);
}

tracktion::TimeRange EditSync::loopRange() const
{
    const auto& project = bus.project();
    return tracktionEdit->tempoSequence.toTime ({ tracktion::BeatPosition(), beatPosition (hocket::project::songEnd (project), project.meter) });
}

//==============================================================================
void EditSync::changed (const commands::Change& change)
{
    // The bus announces a command's changes once all its edits have applied,
    // so each is synced against the finished project. Tracktion rebuilds its
    // playback graph on the next message-thread callback and swaps it in
    // without stopping.
    std::visit ([this] (const auto& c)
    {
        using Kind = std::decay_t<decltype (c)>;

        if constexpr (std::is_same_v<Kind, commands::NotesChanged>)
            syncFile (c.file);
        else if constexpr (requires { c.clipId; })
            syncClip (c.clipId);
        else if constexpr (std::is_same_v<Kind, commands::TrackAdded> || std::is_same_v<Kind, commands::TrackRemoved>
                           || std::is_same_v<Kind, commands::TrackMoved> || std::is_same_v<Kind, commands::TrackRenamed>
                           || std::is_same_v<Kind, commands::InstrumentChanged>)
            syncTrack (c.trackId);

        // The tempo, time signature and mix reach the Edit in Milestone 1's
        // step 6. A drum map changes how clips are written, not what they play.
    }, change);

    clearTracktionUndo();

    // Clips added, moved or removed, or a new @length, can move the end of the song.
    tracktionEdit->getTransport().setLoopRange (loopRange());
}

void EditSync::syncTrack (const std::string& trackId)
{
    const auto& project = bus.project();
    const auto* source = hocket::project::findTrack (project, trackId);

    if (source == nullptr)
    {
        removeTrack (trackId);
        return;
    }

    auto found = tracks.find (trackId);

    if (found == tracks.end())
    {
        const auto index = static_cast<std::size_t> (source - project.tracks.data());
        auto track = tracktionEdit->insertNewAudioTrack (tracktion::TrackInsertPoint (nullptr, trackBefore (index)), nullptr);

        if (track == nullptr)
            throw std::runtime_error ("Tracktion couldn't add a track for " + trackId);

        found = tracks.emplace (trackId, SyncedTrack { track, {} }).first;
    }

    found->second.track->setName (juceString (source->name));
    syncDevices (*source, found->second);

    // A track added back by undo brings its clips.
    for (const auto& clip : source->clips)
        if (! clips.contains (clip.id))
            syncClip (clip.id);

    syncTrackOrder();
}

void EditSync::syncDevices (const project::Track& source, SyncedTrack& synced)
{
    // A device's ID stays with its type, so a plugin is only ever made or removed.
    for (auto plugin = synced.plugins.begin(); plugin != synced.plugins.end();)
    {
        if (std::ranges::any_of (source.devices, [&plugin] (const project::Device& device) { return device.id == plugin->first; }))
        {
            ++plugin;
            continue;
        }

        plugin->second->deleteFromParent();
        plugin = synced.plugins.erase (plugin);
    }

    // In signal-chain order, ahead of the volume/pan and meter plugins Tracktion puts on every track.
    int index = 0;

    for (const auto& device : source.devices)
    {
        if (synced.plugins.contains (device.id))
        {
            ++index;
            continue;
        }

        if (auto plugin = createDevice (*tracktionEdit, device))
        {
            synced.track->pluginList.insertPlugin (plugin, index++, nullptr);
            synced.plugins.emplace (device.id, std::move (plugin));
        }
    }
}

void EditSync::syncTrackOrder()
{
    const auto& project = bus.project();
    std::size_t place = 0; // among the Edit's audio tracks

    for (std::size_t i = 0; i < project.tracks.size(); ++i)
    {
        const auto found = tracks.find (project.tracks[i].id);

        if (found == tracks.end())
            continue; // not made yet

        const auto audioTracks = tracktion::getAudioTracks (*tracktionEdit);

        if (place >= static_cast<std::size_t> (audioTracks.size()) || audioTracks[static_cast<int> (place)] != found->second.track.get())
            tracktionEdit->moveTrack (found->second.track, tracktion::TrackInsertPoint (nullptr, trackBefore (i)));

        ++place;
    }
}

tracktion::Track* EditSync::trackBefore (std::size_t index) const
{
    const auto& project = bus.project();

    for (auto i = std::min (index, project.tracks.size()); i-- > 0;)
        if (const auto found = tracks.find (project.tracks[i].id); found != tracks.end())
            return found->second.track.get();

    // The first goes after the Edit's own tracks (tempo, markers and so on), ahead of every audio track.
    tracktion::Track* last = nullptr;

    for (auto* track : tracktion::getTopLevelTracks (*tracktionEdit))
    {
        if (dynamic_cast<tracktion::AudioTrack*> (track) != nullptr)
            break;

        last = track;
    }

    return last;
}

void EditSync::removeTrack (const std::string& trackId)
{
    const auto found = tracks.find (trackId);

    if (found == tracks.end())
        return;

    // Its clips go with it.
    const tracktion::ClipTrack* track = found->second.track.get();
    std::erase_if (clips, [track] (const auto& clip) { return clip.second->getTrack() == track; });

    tracktionEdit->deleteTrack (found->second.track.get());
    tracks.erase (found);
}

void EditSync::syncClip (const std::string& clipId)
{
    const auto& project = bus.project();
    const hocket::project::Track* sourceTrack = nullptr;
    const auto* source = hocket::project::findClip (project, clipId, &sourceTrack);

    if (source == nullptr)
    {
        removeClip (clipId);
        return;
    }

    const auto track = tracks.find (sourceTrack->id);

    // Its track isn't in the Edit yet: making it makes its clips.
    if (track == tracks.end())
    {
        syncTrack (sourceTrack->id);
        return;
    }

    auto found = clips.find (clipId);

    // Moved to another track: this one goes, and a new one is made there.
    if (found != clips.end() && found->second->getTrack() != track->second.track.get())
    {
        removeClip (clipId);
        found = clips.end();
    }

    if (found == clips.end())
    {
        const tracktion::BeatRange beats { beatPosition (source->start, project.meter),
                                           beatPosition (hocket::project::clipEnd (project, *source), project.meter) };
        auto midiClip = track->second.track->insertMIDIClip (juceString (clipId), tracktionEdit->tempoSequence.toTime (beats), nullptr);

        if (midiClip == nullptr)
            throw std::runtime_error ("Tracktion couldn't add a clip for " + clipId);

        // A clip that uses a proxy sequence plays through Tracktion's MidiNode,
        // which keeps no note state across graph swaps: when a rewrite changes a
        // note that's sounding, the new graph sends the new note's note-off and
        // nothing ever ends the old one. Without the proxy it plays through
        // LoopingMidiNode, which hands its sounding notes to the next graph and
        // ends the ones the rewrite took away. The proxy only saves CPU on
        // quantisation and groove, which Hocket doesn't use. A Tracktion bug:
        // every Tracktion update checks whether this is still needed
        // (DEPENDENCIES.md, "Updating Tracktion").
        midiClip->setUsesProxy (false);
        found = clips.emplace (clipId, midiClip).first;
    }

    writeClip (*found->second, project, *source);
}

void EditSync::removeClip (const std::string& clipId)
{
    const auto found = clips.find (clipId);

    if (found == clips.end())
        return;

    found->second->removeFromParent();
    clips.erase (found);
}

void EditSync::syncFile (const std::string& file)
{
    // Every clip linked to the file. A file that left the project has none:
    // its clips' removals come as changes of their own.
    for (const auto* clip : hocket::project::clipsUsing (bus.project(), file))
        syncClip (clip->id);
}

void EditSync::clearTracktionUndo()

{
    // Tracktion's undo is unused (SPEC §3.3), but some of its setters (tempo,
    // clip position) record steps anyway. JUCE can't be told to keep none.
    tracktionEdit->getUndoManager().clearUndoHistory();
}

//==============================================================================
commands::Result EditSync::play()
{
    auto& transport = tracktionEdit->getTransport();
    playRequested = true;

    if (transport.isPlaying())
        return { true, "already playing" };

    const auto range = loopRange();
    transport.setLoopRange (range);
    transport.looping = true;
    transport.setPosition (range.getStart());
    transport.play (false);

    const auto& project = bus.project();
    return { true, "playing " + notes::formatPosition (0, project.meter) + " to " + notes::formatPosition (hocket::project::songEnd (project), project.meter) + " on a loop" };
}

commands::Result EditSync::stop()
{
    auto& transport = tracktionEdit->getTransport();
    playRequested = false;

    if (! transport.isPlaying())
        return { true, "already stopped" };

    transport.stop (false, false);
    return { true, "stopped" };
}

bool EditSync::isPlaying() const
{
    // Tracktion can stop by itself, e.g. when it rebuilds its device lists.
    return tracktionEdit->getTransport().isPlaying();
}

bool EditSync::stoppedByItself() const
{
    return playRequested && ! isPlaying();
}
}
