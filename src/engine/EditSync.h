#pragma once

// The sync layer (SPEC §3.3): builds a Tracktion Edit from the command bus's
// project, then keeps it in step as commands apply. Hocket's model is the
// source of truth; the Edit is derived state, never read back or saved.
//
// Tracks, devices and clips are matched by their Hocket IDs. Each change the
// bus announces names a track, clip or file; that one is brought in line with
// the model as it is now, whatever the change was, so the order of a
// command's changes doesn't matter. Every track's mix is checked after every
// change, since one track's solo silences the others. A tempo change during
// playback keeps the playhead's place in the bar, and the notes sounding.
//
// It's also the bus's transport. Play loops the whole song.
//
// Message thread only, like the bus.

#include "core/commands/CommandBus.h"

#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace hocket::engine
{
    class EditSync final : private commands::Transport
    {
    public:
        // Builds the Edit, then registers with the bus as a listener and as its
        // transport. The bus must outlive this.
        EditSync (tracktion::Engine& engine, commands::CommandBus& bus);
        ~EditSync() override;

        EditSync (const EditSync&) = delete;
        EditSync& operator= (const EditSync&) = delete;

        tracktion::Edit& edit() { return *tracktionEdit; }

        // What play loops: from the song start to the end of the last clip,
        // rounded up to a bar line, and at least one bar.
        tracktion::TimeRange loopRange() const;

        // Whether playback has stopped though the last transport command was
        // play. Tracktion stops by itself when it rebuilds its device lists: a
        // MIDI or audio device plugged in, unplugged, or reset by its driver.
        bool stoppedByItself() const;

    private:
        // A Hocket track in the Edit, the plugins its devices made, by device
        // ID, and the volume plugin Tracktion gave it, which plays its mix.
        struct SyncedTrack
        {
            tracktion::AudioTrack::Ptr track;
            std::map<std::string, tracktion::Plugin::Ptr, std::less<>> plugins;
            tracktion::VolumeAndPanPlugin::Ptr volume;
        };

        commands::CommandBus& bus;
        std::unique_ptr<tracktion::Edit> tracktionEdit;
        std::map<std::string, SyncedTrack, std::less<>> tracks;             // by Hocket track ID
        std::map<std::string, tracktion::MidiClip::Ptr, std::less<>> clips; // by Hocket clip ID
        int listenerId = 0;
        bool playRequested = false; // by the last transport command

        void changed (const commands::Change&);

        // Each brings one thing in the Edit in line with the model: makes it,
        // changes it, or removes it.
        void syncTrack (const std::string& trackId);
        void syncClip (const std::string& clipId);
        void syncFile (const std::string& file);

        void syncDevices (const project::Track&, SyncedTrack&);
        void syncTrackOrder();
        void removeTrack (const std::string& trackId);
        void removeClip (const std::string& clipId);

        // Every track's volume, pan, mute and solo, through its Tracktion
        // volume plugin, which smooths changes so they don't click.
        void syncMix();

        // The tempo, with every clip moved to keep its beats, and the
        // playhead's place and loop changed to match in the same instant.
        void syncTempo();

        // The Tracktion track a Hocket track at this place in the display order goes after.
        tracktion::Track* trackBefore (std::size_t index) const;

        // Sets the loop to loopRange(), and hands it to the playhead at once.
        void updateLoop();

        // The playhead while the transport loops (as play() sets it to), and
        // the sample rate it counts in; none before the first play.
        std::pair<tracktion::graph::PlayHead*, double> loopingPlayHead() const;

        // JUCE's lock around each audio block. Held on the message thread,
        // only for a few microseconds, so a change to the playhead and what
        // it reads happens between blocks.
        juce::CriticalSection& audioCallbackLock() const;

        void clearTracktionUndo();

        commands::Result play() override;
        commands::Result stop() override;
        bool isPlaying() const override;
    };
}
