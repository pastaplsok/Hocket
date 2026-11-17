#include "engine/EditSync.h"

#include "core/notes/NotesSyntax.h"
#include "core/units/Units.h"

#include <tracktion_graph/tracktion_graph.h>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <stdexcept>

namespace hocket::engine
{
namespace
{
    // How often, with loop off, playback checks whether it has reached the end.
    constexpr int endCheckIntervalMs = 5;

    juce::String juceString (const std::string& utf8)
    {
        return juce::String::fromUTF8 (utf8.data(), static_cast<int> (utf8.size()));
    }

    // Hocket counts ticks, 960 per quarter note; Tracktion counts beats, and
    // its beats are quarter notes, since the Edit's time signature stays at
    // Tracktion's 4/4 whatever the song's is (see the constructor). Its BPM is
    // quarter notes per minute, as Hocket's is (SPEC §4.4).
    tracktion::BeatPosition beatPosition (notes::Ticks ticks)
    {
        return tracktion::BeatPosition::fromBeats (static_cast<double> (ticks) / static_cast<double> (notes::ticksPerQuarterNote));
    }

    tracktion::BeatDuration beatDuration (notes::Ticks ticks)
    {
        return tracktion::BeatDuration::fromBeats (static_cast<double> (ticks) / static_cast<double> (notes::ticksPerQuarterNote));
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

    // A track's mix (SPEC §4.4) as Tracktion's volume plugin takes it: its
    // fader position and pan.
    struct VolumePluginSettings
    {
        float faderPosition = 0.0f; // 0 is silence, 1 is +6 dB
        float pan = 0.0f;           // -1 to 1
    };

    // Hocket's pan is a balance control: the near channel stays at the
    // track's volume, and the far one goes down linearly in gain, to (1 - |b|)
    // of it. The volume plugin's linear pan law gives left g(1 - p) and right
    // g(1 + p), which is that when p = b / (2 - |b|) and g = volume / (1 + |p|).
    // At L50, p is -1/3 and g 0.75 of the volume: left at the volume, right
    // at half. A track that shouldn't be heard (muted, or another is soloed)
    // gets no gain, so the plugin's 15 ms smoothing fades it out and back in.
    VolumePluginSettings volumePluginSettings (const project::Track& track, bool audible)
    {
        const auto balance = static_cast<double> (track.pan) / static_cast<double> (units::maxPan);
        const auto pan = balance / (2.0 - std::abs (balance));
        const auto gain = audible ? std::pow (10.0, track.volume / 20.0) / (1.0 + std::abs (pan)) : 0.0; // -inf dB is 0

        // From a gain, not dB: the plugin's dB conversion treats anything at
        // -100 dB or under as silence, and -96 dB panned hard is lower.
        return { tracktion::gainToVolumeFaderPosition (static_cast<float> (gain)), static_cast<float> (pan) };
    }

    tracktion::BeatRange clipBeats (const project::Project& project, const project::Clip& clip)
    {
        return { beatPosition (clip.start), beatPosition (hocket::project::clipEnd (project, clip)) };
    }

    // Puts the Tracktion clip where the model's is, in time at the Edit's tempo.
    void placeClip (tracktion::MidiClip& target, const project::Project& project, const project::Clip& clip)
    {
        target.setPosition ({ target.edit.tempoSequence.toTime (clipBeats (project, clip)) });
    }

    // Makes the Tracktion clip match the model's: position, length and notes.
    void writeClip (tracktion::MidiClip& target, const project::Project& project, const project::Clip& clip)
    {
        placeClip (target, project, clip);

        // Undo is Hocket's job (SPEC §3.3), so no UndoManager. Notes that run past
        // the clip end are cut off there by Tracktion, as SPEC §5.4 says.
        auto& sequence = target.getSequence();
        sequence.clear (nullptr);

        for (const auto& note : hocket::project::notesOf (project, clip).notes)
            sequence.addNote (note.pitch, beatPosition (note.start), beatDuration (note.length), note.velocity, 0, nullptr);
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
        // Tracktion's default is -3 dB. At 0 dB, a track at "0dB" and "C"
        // reaches a render exactly as its instrument made it (SPEC §4.4).
        .defaultMasterVolumedB = 0.0f,
    });

    if (tracktionEdit == nullptr)
        throw std::runtime_error ("Tracktion couldn't create an Edit");

    // Tracktion can record a parameter's changes during playback as
    // automation, and then play that back: a volume set while playing would
    // be overridden on the next pass. Its tracks start in read mode, which
    // records nothing, but writing is on by default, and one setting away
    // from recording; Hocket's mix and automation come only from commands.
    tracktionEdit->getAutomationRecordManager().setWritingAutomation (false);

    // The Edit's time signature stays at Tracktion's 4/4, so its beats are
    // quarter notes, and Hocket works out its own bars. A new time signature
    // then moves nothing in time, as SPEC §4.4 has it. Changing the Edit's
    // would make Tracktion's audio thread put the playhead back on the beat it
    // was on, as it does after a tempo change, and that would undo a wrap
    // made in the same instant (updateLoop). Anything that reads the
    // signature or the bar from Tracktion needs the Edit's to be right: a
    // metronome, and from Phase 2 the VST3 plugins it hosts, which take the
    // time signature and bar position from the host. Revisit this before
    // either (PROGRESS.md, Loose ends).
    syncTempo();

    for (const auto& track : project.tracks)
        syncTrack (track.id);

    syncMix();
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
    const auto range = commands::playRange (bus.project(), bus.transportSettings());
    return tracktionEdit->tempoSequence.toTime ({ beatPosition (range.start), beatPosition (range.end) });
}

tracktion::TimePosition EditSync::timeOf (notes::Ticks position) const
{
    return tracktionEdit->tempoSequence.toTime (beatPosition (position));
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
        else if constexpr (std::is_same_v<Kind, commands::TempoChanged>)
            syncTempo();
        else if constexpr (requires { c.clipId; })
            syncClip (c.clipId);
        else if constexpr (std::is_same_v<Kind, commands::TrackAdded> || std::is_same_v<Kind, commands::TrackRemoved>
                           || std::is_same_v<Kind, commands::TrackMoved> || std::is_same_v<Kind, commands::TrackRenamed>
                           || std::is_same_v<Kind, commands::InstrumentChanged>)
            syncTrack (c.trackId);

        // A MixChanged is handled below, with every track's. A time signature
        // moves nothing in time (the constructor says why the Edit's own stays
        // 4/4); the @length it rewrites comes as a NotesChanged, and where the
        // song now ends is handled below. A drum map changes how clips are
        // written, not what they play.
    }, change);

    // One track's solo changes whether the others are heard, and so does
    // removing a soloed track, so every track's mix is checked. Only what
    // differs is set.
    syncMix();
    clearTracktionUndo();

    // Clips added, moved or removed, a new @length or a new time signature
    // can move the end of the song.
    updateLoop();
}

void EditSync::updateLoop()
{
    auto& transport = tracktionEdit->getTransport();
    const auto range = loopRange();
    const auto looping = bus.transportSettings().looping;

    if (transport.getLoopRange() == range && transport.looping.get() == looping)
        return;

    // The transport's timer hands these to the playhead every tenth tick at
    // 50 Hz, so they must agree with what's handed over below.
    transport.setLoopRange (range);
    transport.looping = looping;

    // Until then it plays on past the old end, so hand them over now.
    const auto [playHead, sampleRate] = currentPlayHead();

    if (playHead == nullptr)
        return;

    // When the playhead is now outside what plays (the song ended before it,
    // or a loop region was set elsewhere), it wraps as if the loop had always
    // been this one: the place within it, by remainder, counted from its
    // start. A loop of whole bars keeps the same beat of the bar, and the
    // pulse carries on (Lee's decision, 2026-11-12; SPEC §3.3). Tracktion's
    // own setLoopRange clamps the place to the loop, so the place is set
    // here, while no audio block can start. With loop off, the playhead plays
    // on from where it is, and timerCallback stops it at the end.
    const auto loop = tracktion::toSamples (range, sampleRate);
    const juce::ScopedLock audioLock (audioCallbackLock());
    const auto place = playHead->getPosition();
    const auto wrapped = ! looping || loop.contains (place)
                           ? place
                           : loop.getStart() + juce::negativeAwareModulo (place - loop.getStart(), std::max<std::int64_t> (1, loop.getLength()));

    // The playhead counts its place from where it was last set, by remainder
    // of the loop's length, so the place is set even when it doesn't move:
    // with looping off, so the old loop doesn't clamp it, and before the new
    // loop is on. Not moving, it's set without telling Tracktion's players it
    // jumped, or they'd end the notes sounding and strike them again.
    // Wrapping, it did jump, and they're told: the notes sounding end, and
    // those on at the new place start.
    playHead->setLoopRange (false, loop, false);

    if (wrapped == place)
        playHead->overridePosition (place);
    else
        playHead->setPosition (wrapped);

    if (looping)
        playHead->setLoopRange (true, loop, false);
}

void EditSync::syncTempo()
{
    auto& sequence = tracktionEdit->tempoSequence;
    auto& tempo = *sequence.getTempo (0);
    const auto bpm = bus.project().tempo;

    if (tempo.getBpm() == bpm)
        return;

    {
        // Tracktion's audio thread reads the Edit's tempo map and the
        // playhead's loop directly, and the playhead counts its place in
        // samples. So the playhead's place and loop change with the tempo,
        // while no audio block can start: the playhead keeps its beat, so its
        // place in the bar, and the loop's new length in samples holds the
        // song. An audio block in between would wrap the place at the old
        // loop's end, or clamp it there. Tracktion takes this lock itself to
        // swap in a new tempo map; this holds it a few microseconds longer
        // (CLAUDE.md: the one exception to the no-locks rule).
        //
        // The tempo setting changes under the lock too: once it has, the
        // next call that reads the tempo map (toBeats, toTime) rebuilds the
        // map first, so the beat is read before, and nothing may read the map
        // between the two outside the lock.
        const juce::ScopedLock audioLock (audioCallbackLock());
        const auto [playHead, sampleRate] = currentPlayHead();
        const auto beat = playHead != nullptr ? sequence.toBeats (tracktion::TimePosition::fromSamples (playHead->getPosition(), sampleRate))
                                              : tracktion::BeatPosition();

        // Clips move below, by Hocket's positions, not by Tracktion's remap.
        tempo.set (tempo.getStartBeat(), bpm, tempo.getCurve(), false);
        sequence.updateTempoData();

        // Without telling Tracktion's players it jumped, so held notes carry
        // on. Tracktion's audio thread also moves the playhead when it sees
        // the new tempo map, to keep the beat its last block ended on, so it
        // lands here too. The loop first, since the place is clamped to it;
        // with loop off there's none, and timerCallback stops at the end.
        if (playHead != nullptr)
        {
            if (playHead->isLooping())
                playHead->setLoopRange (true, tracktion::toSamples (loopRange(), sampleRate), false);

            playHead->overridePosition (tracktion::toSamples (sequence.toTime (beat), sampleRate));
        }
    }

    // Clips keep their beats, so they move in time. Tracktion's players keep
    // the notes sounding, since what plays at the playhead's beat is the same.
    const auto& project = bus.project();

    for (const auto& [clipId, clip] : clips)
        if (const auto* source = hocket::project::findClip (project, clipId))
            placeClip (*clip, project, *source);

    tracktionEdit->getTransport().setLoopRange (loopRange());
}

std::pair<tracktion::graph::PlayHead*, double> EditSync::currentPlayHead() const
{
    auto* context = tracktionEdit->getTransport().getCurrentPlaybackContext();
    auto* playHead = context != nullptr ? context->getNodePlayHead() : nullptr;
    return { playHead, playHead != nullptr ? context->getSampleRate() : 0.0 };
}

juce::CriticalSection& EditSync::audioCallbackLock() const
{
    return tracktionEdit->engine.getDeviceManager().deviceManager.getAudioCallbackLock();
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

        // Tracktion gives every new track a volume plugin; its pan law is set
        // here, not engine-wide, before it ever plays (syncMix).
        tracktion::VolumeAndPanPlugin::Ptr volume = track->getVolumePlugin();

        if (volume == nullptr)
            throw std::runtime_error ("Tracktion added no volume plugin to the track for " + trackId);

        volume->setPanLaw (tracktion::PanLawLinear);
        found = tracks.emplace (trackId, SyncedTrack { track, {}, volume }).first;
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

void EditSync::syncMix()
{
    const auto& project = bus.project();
    const auto anySoloed = std::ranges::any_of (project.tracks, &project::Track::solo);

    for (const auto& source : project.tracks)
    {
        const auto found = tracks.find (source.id);

        if (found == tracks.end())
            continue; // not made yet

        // While any track is soloed, only soloed tracks are heard, and a muted
        // track is never heard, soloed or not (SPEC §4.4).
        const auto audible = ! source.mute && (! anySoloed || source.solo);
        const auto settings = volumePluginSettings (source, audible);
        auto& volume = *found->second.volume;

        // The parameters, not the plugin's setters: setPan() rounds a pan
        // under 0.005 to the centre, and L1 comes to 0.00503, only just over.
        // The audio thread reads their values, which are atomic, and smooths
        // the gains it makes from them.
        if (volume.volParam->getCurrentValue() != settings.faderPosition)
            volume.volParam->setParameter (settings.faderPosition, juce::sendNotification);

        if (volume.panParam->getCurrentValue() != settings.pan)
            volume.panParam->setParameter (settings.pan, juce::sendNotification);
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

    // Moved: this one goes, and a new one is made in its place. Tracktion's
    // LoopingMidiNode hands a clip's sounding notes to the next graph, so a
    // clip moved in place would keep its note sounding until its new start;
    // a clip removed takes its notes with it, so a moved clip's note ends
    // when it moves (step 5's live tests). Only a new start does this: a new
    // @length keeps the clip, so a note held across it isn't cut.
    if (found != clips.end())
    {
        const auto movedTrack = found->second->getTrack() != track->second.track.get();
        const auto movedStart = std::abs (found->second->getStartBeat().inBeats() - beatPosition (source->start).inBeats()) > 1.0e-9;

        if (movedTrack || movedStart)
        {
            removeClip (clipId);
            found = clips.end();
        }
    }

    if (found == clips.end())
    {
        auto midiClip = track->second.track->insertMIDIClip (juceString (clipId), tracktionEdit->tempoSequence.toTime (clipBeats (project, *source)), nullptr);

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
void EditSync::play()
{
    auto& transport = tracktionEdit->getTransport();
    const auto& settings = bus.transportSettings();
    const auto start = timeOf (commands::startPosition (bus.project(), settings));
    playRequested = true;

    transport.setLoopRange (loopRange());
    transport.looping = settings.looping;
    transport.setPosition (start);
    transport.play (false);

    // Tracktion starts a loop at its start when the place is in its last
    // 0.1 s, and a seek can put it there.
    if (transport.getPosition() != start)
        transport.setPosition (start);

    // Loop off: play runs to the end, and stops (SPEC §3.1).
    if (! settings.looping)
        startTimer (endCheckIntervalMs);
}

void EditSync::stop()
{
    playRequested = false;
    stopTimer();
    tracktionEdit->getTransport().stop (false, false);
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

notes::Ticks EditSync::playheadPosition() const
{
    const auto [playHead, sampleRate] = currentPlayHead();

    if (playHead == nullptr)
        return commands::startPosition (bus.project(), bus.transportSettings());

    const auto beats = tracktionEdit->tempoSequence.toBeats (tracktion::TimePosition::fromSamples (playHead->getPosition(), sampleRate));
    return std::max<notes::Ticks> (0, std::llround (beats.inBeats() * static_cast<double> (notes::ticksPerQuarterNote)));
}

void EditSync::jumpTo (notes::Ticks position)
{
    const auto [playHead, sampleRate] = currentPlayHead();

    if (playHead == nullptr)
        return;

    // A jump: Tracktion's players end the notes sounding, and strike those on
    // at the new place. The bus has put it inside what plays, so the loop
    // doesn't clamp it.
    playHead->setPosition (tracktion::toSamples (timeOf (position), sampleRate));
}

void EditSync::loopChanged()
{
    updateLoop();

    if (isPlaying() && ! bus.transportSettings().looping)
        startTimer (endCheckIntervalMs);
}

void EditSync::timerCallback()
{
    auto& transport = tracktionEdit->getTransport();

    if (! transport.isPlaying() || bus.transportSettings().looping)
    {
        stopTimer();
        return;
    }

    const auto [playHead, sampleRate] = currentPlayHead();

    if (playHead == nullptr || playHead->getPosition() < tracktion::toSamples (loopRange().getEnd(), sampleRate))
        return;

    // Asked for, so not a stop by itself. By now the playhead may be a block
    // or two past the end (Loose ends).
    stopTimer();
    playRequested = false;
    transport.stop (false, false);
}
}
