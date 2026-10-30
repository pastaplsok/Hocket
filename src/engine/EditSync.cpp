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

    // Hocket counts ticks (960 per beat); Tracktion counts beats.
    tracktion::BeatPosition beatPosition (notes::Ticks ticks)
    {
        return tracktion::BeatPosition::fromBeats (static_cast<double> (ticks) / notes::ticksPerBeat);
    }

    tracktion::BeatDuration beatDuration (notes::Ticks ticks)
    {
        return tracktion::BeatDuration::fromBeats (static_cast<double> (ticks) / notes::ticksPerBeat);
    }

    tracktion::Plugin::Ptr createDevice (tracktion::Edit& edit, const project::Device& device)
    {
        switch (device.type)
        {
            case project::DeviceType::fourOsc:
                return edit.getPluginCache().createNewPlugin (tracktion::FourOscPlugin::xmlTypeName, {});
        }

        return {};
    }

    // Makes the Tracktion clip match the model's: position, length and notes.
    void writeClip (tracktion::MidiClip& target, const project::Clip& clip, notes::Meter meter)
    {
        const tracktion::BeatRange beats { beatPosition (clip.start), beatPosition (hocket::project::clipEnd (clip, meter)) };
        target.setPosition ({ target.edit.tempoSequence.toTime (beats) });

        // Undo is Hocket's job (SPEC §3.3), so no UndoManager. Notes that run past
        // the clip end are cut off there by Tracktion, as SPEC §5.4 says.
        auto& sequence = target.getSequence();
        sequence.clear (nullptr);

        for (const auto& note : clip.content.notes)
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
        .numAudioTracks = static_cast<std::uint32_t> (project.tracks.size()),
    });

    if (tracktionEdit == nullptr)
        throw std::runtime_error ("Tracktion couldn't create an Edit");

    auto& tempo = tracktionEdit->tempoSequence;
    tempo.getTempo (0)->setBpm (project.tempo);
    tempo.getTimeSig (0)->setStringTimeSig (juceString (std::to_string (project.meter.beatsPerBar) + "/4"));

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
            const tracktion::BeatRange beats { beatPosition (clip.start), beatPosition (hocket::project::clipEnd (clip, project.meter)) };
            auto midiClip = track.insertMIDIClip (juceString (clip.id), tempo.toTime (beats), nullptr);
            writeClip (*midiClip, clip, project.meter);
            clips.emplace (clip.id, midiClip);
        }
    }

    clearTracktionUndo();

    listenerId = bus.addListener ([this] (const commands::Change& change)
    {
        if (const auto* changed = std::get_if<commands::ClipContentChanged> (&change))
            clipContentChanged (changed->clipId);
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
    return tracktionEdit->tempoSequence.toTime ({ tracktion::BeatPosition(), beatPosition (hocket::project::songEnd (bus.project())) });
}

void EditSync::clipContentChanged (const std::string& clipId)
{
    const auto target = clips.find (clipId);
    const auto* clip = hocket::project::findClip (bus.project(), clipId);

    // Milestone 0 has no commands that add or remove clips.
    assert (target != clips.end() && clip != nullptr);

    // Tracktion rebuilds its playback graph on the next message-thread callback
    // and swaps it in without stopping: notes still in the clip keep sounding,
    // removed ones get note-offs (LoopingMidiNode::prepareToPlay).
    writeClip (*target->second, *clip, bus.project().meter);
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

    if (! transport.isPlaying())
        return { true, "already stopped" };

    transport.stop (false, false);
    return { true, "stopped" };
}
}
