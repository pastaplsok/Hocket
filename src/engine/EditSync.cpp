#include "engine/EditSync.h"

#include "core/notes/NotesSyntax.h"

#include <algorithm>
#include <cassert>
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
        .numAudioTracks = static_cast<std::uint32_t> (project.tracks.size()),
    });

    if (tracktionEdit == nullptr)
        throw std::runtime_error ("Tracktion couldn't create an Edit");

    auto& tempo = tracktionEdit->tempoSequence;
    tempo.getTempo (0)->setBpm (project.tempo);
    tempo.getTimeSig (0)->setStringTimeSig (juceString (project.meter.text()));

    // Tracktion makes the tracks (Options::numAudioTracks); they're in display order.
    const auto tracks = tracktion::getAudioTracks (*tracktionEdit);
    assert (static_cast<std::size_t> (tracks.size()) == project.tracks.size());

    for (std::size_t i = 0; i < project.tracks.size(); ++i)
    {
        const auto& source = project.tracks[i];
        auto& track = *tracks[static_cast<int> (i)];
        track.setName (juceString (source.name));

        // Ahead of the volume/pan and meter plugins Tracktion puts on every track.
        for (std::size_t d = 0; d < source.devices.size(); ++d)
            track.pluginList.insertPlugin (createDevice (*tracktionEdit, source.devices[d]), static_cast<int> (d), nullptr);

        for (const auto& clip : source.clips)
        {
            const tracktion::BeatRange beats { beatPosition (clip.start, project.meter),
                                               beatPosition (hocket::project::clipEnd (project, clip), project.meter) };
            auto midiClip = track.insertMIDIClip (juceString (clip.id), tempo.toTime (beats), nullptr);

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
            writeClip (*midiClip, project, clip);
            clips.emplace (clip.id, midiClip);
        }
    }

    clearTracktionUndo();

    listenerId = bus.addListener ([this] (const commands::Change& change)
    {
        if (const auto* changed = std::get_if<commands::NotesChanged> (&change))
            notesChanged (changed->file);
    });

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

void EditSync::notesChanged (const std::string& file)
{
    const auto& project = bus.project();

    // Every clip linked to the file. Tracktion rebuilds its playback graph on
    // the next message-thread callback and swaps it in without stopping: notes
    // still in a clip keep sounding, removed ones get note-offs
    // (LoopingMidiNode, as the clips don't use a proxy; see the constructor).
    for (const auto* clip : hocket::project::clipsUsing (project, file))
    {
        const auto target = clips.find (clip->id);

        // No command adds or removes clips yet.
        assert (target != clips.end());
        writeClip (*target->second, project, *clip);
    }

    clearTracktionUndo();

    // A new @length can move the end of the song.
    tracktionEdit->getTransport().setLoopRange (loopRange());
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
