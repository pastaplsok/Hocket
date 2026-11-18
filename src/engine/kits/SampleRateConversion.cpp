#include "engine/kits/SampleRateConversion.h"

#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

// Tracktion compiles libsamplerate's C files as C++ inside namespace
// tracktion::src, with its sinc converters on (tracktion_engine_playback.cpp).
// The header, included in that same namespace, declares the very functions
// hocket_juce holds; it includes nothing itself. DEPENDENCIES.md's "Updating
// Tracktion" checks this still holds.
namespace tracktion::src
{
#include <3rd_party/libsamplerate/samplerate.h>
}

namespace hocket::engine
{
std::vector<float> convertSampleRate (std::span<const float> samples, double fromRate, double toRate)
{
    if (! (fromRate > 0.0) || ! (toRate > 0.0))
        throw std::runtime_error ("can't convert samples from " + std::to_string (fromRate) + " Hz to " + std::to_string (toRate) + " Hz");

    if (fromRate == toRate || samples.empty())
        return { samples.begin(), samples.end() };

    namespace src = tracktion::src;
    const auto ratio = toRate / fromRate;

    if (src::src_is_valid_ratio (ratio) == 0)
        throw std::runtime_error ("libsamplerate can't convert from " + std::to_string (fromRate) + " Hz to " + std::to_string (toRate)
                                  + " Hz: the ratio is out of its range");

    if (samples.size() > static_cast<std::size_t> (std::numeric_limits<long>::max() / 4))
        throw std::runtime_error ("a sample of " + std::to_string (samples.size()) + " frames is too long to convert");

    const auto outputLength = static_cast<std::size_t> (std::llround (static_cast<double> (samples.size()) * ratio));
    std::vector<float> output (outputLength, 0.0f);

    int error = 0;
    const std::unique_ptr<src::SRC_STATE, decltype (&src::src_delete)> state (src::src_new (src::SRC_SINC_BEST_QUALITY, 1, &error), &src::src_delete);

    if (state == nullptr)
        throw std::runtime_error (std::string ("libsamplerate couldn't start: ") + src::src_strerror (error));

    // All the input at once, marked as the end, then again until the
    // filter's tail is out: each call makes what fits.
    std::size_t consumed = 0;
    std::size_t produced = 0;

    while (produced < outputLength)
    {
        src::SRC_DATA data {};
        data.data_in = samples.data() + consumed;
        data.input_frames = static_cast<long> (samples.size() - consumed);
        data.data_out = output.data() + produced;
        data.output_frames = static_cast<long> (outputLength - produced);
        data.end_of_input = 1;
        data.src_ratio = ratio;

        if (error = src::src_process (state.get(), &data); error != 0)
            throw std::runtime_error (std::string ("libsamplerate couldn't convert the samples: ") + src::src_strerror (error));

        consumed += static_cast<std::size_t> (data.input_frames_used);
        produced += static_cast<std::size_t> (data.output_frames_gen);

        // It has nothing more to give: the rest of the length stays silent.
        if (data.output_frames_gen == 0)
            break;
    }

    return output;
}
}
