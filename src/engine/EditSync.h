#pragma once

// The sync layer (SPEC §3.3): builds a Tracktion Edit from the command bus's
// project, then keeps it in step as commands apply. Hocket's model is the
// source of truth; the Edit is derived state, never read back or saved.
//
// It's also the bus's transport. In Milestone 0, play loops the whole song.
//
// Message thread only, like the bus.

#include "core/commands/CommandBus.h"

#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

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

    private:
        commands::CommandBus& bus;
        std::unique_ptr<tracktion::Edit> tracktionEdit;
        std::map<std::string, tracktion::MidiClip::Ptr, std::less<>> clips; // by Hocket clip ID
        int listenerId = 0;

        void clipContentChanged (const std::string& clipId);
        void clearTracktionUndo();

        commands::Result play() override;
        commands::Result stop() override;
        bool isPlaying() const override;
    };
}
