#pragma once

// hocket-headless --play's master level (SPEC §9): a gain on everything
// Tracktion sends to the audio device, and a meter of what reaches the driver.
// Some drivers bypass the Windows volume slider, so without it playback can be
// far louder than other audio at the same setting (PROGRESS.md, Loose ends).
//
// It sits after the Edit, as Tracktion's global output processor, so it isn't
// part of the project: offline renders don't include it, and it isn't saved.
// The master bus (Milestone 5) takes over from it.

#include "core/commands/Transport.h"

#include <tracktion_engine/tracktion_engine.h>

#include <cstdint>
#include <memory>
#include <string>

namespace hocket::engine
{
    // --play's level without --master-volume, in dB: the project as it's mixed
    // (Lee, 2026-11-20). It was -15 dB, from the night ASIO4ALL played the
    // demo far too loud, until the bundled electronic kit (its kick at -12 dBFS
    // at v127) was barely audible on the Magni with every volume at 100%. The
    // Milestone 0 demo peaks at about -8.2 dBFS (rendered on 2026-11-16); a
    // five-note chord at full velocity on one 4OSC peaks at +11.3 to +13.5 dBFS
    // and clips, which the exit report warns of, suggesting a --master-volume.
    inline constexpr double defaultMasterVolume = 0.0;

    // Also the bus's output meter (SPEC §6.4): a second peak and count, which
    // restart each time a transport play starts.
    class MasterOutput final : public commands::OutputMeter
    {
    public:
        // From now on, everything Tracktion plays on the device is scaled by
        // `volume`, in dB as units::volume() reads it (-inf is silence), then
        // metered. Message thread. Destroy it before the engine.
        MasterOutput (tracktion::DeviceManager&, double volume);
        ~MasterOutput();

        MasterOutput (const MasterOutput&) = delete;
        MasterOutput& operator= (const MasterOutput&) = delete;

        double volume() const { return volumeDb; }

        // Since this was made, after the level: the loudest sample sent to the
        // device, on any channel (1.0 is full scale), and how many samples
        // went over full scale, which the driver cuts off.
        float peak() const;
        std::uint64_t clippedSamples() const;

        commands::OutputLevel levelSincePlay() const override;
        void restart() override;

        // Shared with the processor, which the device manager owns.
        struct Meter;

    private:
        tracktion::DeviceManager& devices;
        double volumeDb;
        std::shared_ptr<Meter> meter;
    };

    // For the end of a run: "peak output level -23.1 dBFS", or "the output was silent".
    std::string describePeak (float peak);

    // When the output clipped: how much, and a --master-volume that would have
    // kept the peak under full scale. Empty when nothing clipped.
    std::string clippingWarning (float peak, std::uint64_t clippedSamples, double volume);
}
