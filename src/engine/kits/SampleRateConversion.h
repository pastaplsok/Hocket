#pragma once

// Converting a kit's samples to the device's sample rate, once, as the kit
// loads or the rate changes (SPEC §8.1), so the audio thread only copies them.

#include <span>
#include <vector>

namespace hocket::engine
{
    // One channel of samples at fromRate, as it would be at toRate: the same
    // length in time (rounded to the nearest sample), band-limited so nothing
    // above the lower rate's Nyquist frequency folds back, and time-aligned, so
    // a hit starts where it did. At equal rates the samples come back as they
    // are, bit for bit.
    //
    // It uses libsamplerate's best sinc converter (144 dB SNR, 96% of the
    // band), which Tracktion bundles and builds into hocket_juce. It
    // allocates and takes milliseconds, so it never runs on the audio thread.
    // Throws std::runtime_error if libsamplerate refuses.
    std::vector<float> convertSampleRate (std::span<const float> samples, double fromRate, double toRate);
}
