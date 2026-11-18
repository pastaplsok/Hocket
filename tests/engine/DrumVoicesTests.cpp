// The drum sampler's real-time core (SPEC §5.2, §8.1): one-shot hits from a
// PlayableKit, with choke groups, handed kits without locks, and nothing
// allocated or freed on the audio thread. Samples here are made up: a ramp
// or a constant, so every output sample can be worked out exactly.

#include "AllocationCounter.h"
#include "engine/devices/DrumVoices.h"

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace hocket;
using engine::DrumEvent;
using engine::DrumVoices;
using engine::PlayableKit;
using engine::SampleAudio;

namespace
{
    constexpr double rate = 44100.0;
    constexpr int fadeSamples = 441; // 10 ms at 44.1 kHz

    std::shared_ptr<const SampleAudio> mono (std::vector<float> samples)
    {
        return std::make_shared<SampleAudio> (SampleAudio { { std::move (samples) } });
    }

    std::shared_ptr<const SampleAudio> constant (float value, std::size_t length)
    {
        return mono (std::vector<float> (length, value));
    }

    // 1/length, 2/length, ... 1: each sample tells where in the sample it is.
    std::vector<float> rampSamples (std::size_t length)
    {
        std::vector<float> samples (length);

        for (std::size_t i = 0; i < length; ++i)
            samples[i] = static_cast<float> (i + 1) / static_cast<float> (length);

        return samples;
    }

    std::unique_ptr<PlayableKit> kitOf (std::vector<PlayableKit::Piece> pieces)
    {
        return std::make_unique<PlayableKit> (rate, std::move (pieces));
    }

    DrumEvent hit (int offset, int note, int velocity = 127)
    {
        return { DrumEvent::Kind::hit, offset, note, velocity };
    }

    struct Stereo
    {
        std::vector<float> left;
        std::vector<float> right;
    };

    // Renders `length` samples in blocks of the sizes given (repeated), with
    // each event at its offset from the start: as a host would, block by block.
    Stereo render (DrumVoices& voices, int length, const std::vector<DrumEvent>& events, std::vector<int> blockSizes = { 512 })
    {
        Stereo out { std::vector<float> (static_cast<std::size_t> (length)), std::vector<float> (static_cast<std::size_t> (length)) };
        std::vector<DrumEvent> blockEvents;
        blockEvents.reserve (events.size());
        int start = 0;

        for (std::size_t block = 0; start < length; ++block)
        {
            const auto size = std::min (blockSizes[block % blockSizes.size()], length - start);
            blockEvents.clear();

            for (auto event : events)
            {
                if (event.offset >= start && event.offset < start + size)
                {
                    event.offset -= start;
                    blockEvents.push_back (event);
                }
            }

            voices.process (out.left.data() + start, out.right.data() + start, size, blockEvents);
            start += size;
        }

        return out;
    }

    double dB (double gain)
    {
        return 20.0 * std::log10 (gain);
    }
}

TEST_CASE ("drum voices: the velocity curve is (v/127)², so v100 is -4.2 dB, v64 -11.9 dB and v28 -26 dB")
{
    CHECK (engine::velocityGain (127) == 1.0f);
    CHECK (engine::velocityGain (0) == 0.0f);
    CHECK (dB (engine::velocityGain (100)) == doctest::Approx (-4.15).epsilon (0.01));
    CHECK (dB (engine::velocityGain (64)) == doctest::Approx (-11.95).epsilon (0.01));
    CHECK (dB (engine::velocityGain (28)) == doctest::Approx (-26.13).epsilon (0.01));
    CHECK (engine::velocityGain (200) == 1.0f);
}

TEST_CASE ("drum voices: a hit plays its sample from its exact sample, at its velocity's gain and the piece's level")
{
    const auto ramp = rampSamples (100);
    DrumVoices voices;
    voices.setKit (kitOf ({ { 36, mono (ramp), 0.5f, -1 } }));

    const auto out = render (voices, 300, { hit (10, 36, 64), hit (150, 36) });
    const auto gainAt64 = 0.5f * engine::velocityGain (64);

    for (int n = 0; n < 300; ++n)
    {
        INFO ("sample " << n);
        const auto index = static_cast<std::size_t> (n);
        const auto expected = n >= 10 && n < 110    ? ramp[index - 10] * gainAt64
                            : n >= 150 && n < 250   ? ramp[index - 150] * 0.5f
                                                    : 0.0f;
        CHECK (out.left[index] == expected);
        CHECK (out.right[index] == expected);
    }

    CHECK (voices.counts().hits == 2);
}

TEST_CASE ("drum voices: a stereo sample keeps its channels")
{
    DrumVoices voices;
    voices.setKit (kitOf ({ { 38, std::make_shared<SampleAudio> (SampleAudio { { std::vector<float> (50, 0.25f), std::vector<float> (50, -0.5f) } }), 1.0f, -1 } }));

    const auto out = render (voices, 100, { hit (0, 38) });
    CHECK (out.left[0] == 0.25f);
    CHECK (out.right[0] == -0.5f);
    CHECK (out.left[49] == 0.25f);
    CHECK (out.left[50] == 0.0f);
}

TEST_CASE ("drum voices: note-offs, all-notes-off and notes with no piece change nothing; hits ring to their end")
{
    const auto makeVoices = []
    {
        auto voices = std::make_unique<DrumVoices>();
        voices->setKit (kitOf ({ { 36, mono (rampSamples (2000)), 1.0f, -1 }, { 42, constant (0.5f, 3000), 1.0f, 0 } }));
        return voices;
    };

    auto plain = makeVoices();
    auto withOthers = makeVoices();
    const auto expected = render (*plain, 4000, { hit (0, 36), hit (100, 42) });

    // Note-offs are note-ons at velocity 0; note 50 has no piece.
    const auto out = render (*withOthers, 4000, { hit (0, 36), hit (5, 36, 0), hit (100, 42), hit (200, 42, 0), hit (300, 50) });
    CHECK (out.left == expected.left);
    CHECK (out.right == expected.right);
    CHECK (expected.left[2099] != 0.0f); // the hat rings its whole 3000 samples
}

TEST_CASE ("drum voices: blocks of any size play the same as one long block")
{
    std::mt19937 random (3);
    std::vector<DrumEvent> events;

    for (int i = 0; i < 60; ++i)
        events.push_back (hit (std::uniform_int_distribution (0, 19999) (random), std::uniform_int_distribution (0, 3) (random) == 0 ? 46 : 42,
                               std::uniform_int_distribution (1, 127) (random)));

    std::ranges::stable_sort (events, {}, &DrumEvent::offset);

    const auto makeVoices = []
    {
        auto voices = std::make_unique<DrumVoices>();
        voices->setKit (kitOf ({ { 42, mono (rampSamples (900)), 0.7f, 0 }, { 46, mono (rampSamples (5000)), 0.6f, 0 } }));
        return voices;
    };

    auto whole = makeVoices();
    const auto expected = render (*whole, 20000, events, { 20000 });

    for (const auto& sizes : { std::vector<int> { 1 }, std::vector<int> { 64, 7, 512, 33 }, std::vector<int> { 441 } })
    {
        INFO ("block sizes from " << sizes.front());
        auto inBlocks = makeVoices();
        const auto out = render (*inBlocks, 20000, events, sizes);
        CHECK (out.left == expected.left);
        CHECK (out.right == expected.right);
    }
}

TEST_CASE ("drum voices: a choke group cuts its hits, its own piece's too, with a 10 ms fade; other pieces ring on")
{
    // Constant samples, so a fade's shape is the output's.
    const auto pieces = std::vector<PlayableKit::Piece> {
        { 42, constant (0.25f, 10000), 1.0f, 0 }, // hat-closed
        { 46, constant (0.5f, 10000), 1.0f, 0 },  // hat-open
        { 49, constant (0.125f, 10000), 1.0f, -1 }, // crash, in no group
        { 36, constant (0.0625f, 10000), 1.0f, 1 }, // a kick that cuts only itself
    };

    // The fade's gain over the n-th sample since it began: 1 at first, 1/441 at last, then none.
    const auto fade = [] (int n) { return n < fadeSamples ? static_cast<float> (fadeSamples - n) / static_cast<float> (fadeSamples) : 0.0f; };

    SUBCASE ("a closed hat cuts an open one")
    {
        DrumVoices voices;
        voices.setKit (kitOf (pieces));
        const auto out = render (voices, 3000, { hit (0, 46), hit (1000, 42) });

        for (int n = 0; n < 3000; ++n)
        {
            INFO ("sample " << n);
            const auto expected = n < 1000 ? 0.5f : 0.5f * fade (n - 1000) + 0.25f;
            CHECK (out.left[static_cast<std::size_t> (n)] == doctest::Approx (expected).epsilon (1e-6));
        }
    }

    SUBCASE ("an open hat struck again restarts")
    {
        DrumVoices voices;
        voices.setKit (kitOf (pieces));
        const auto out = render (voices, 3000, { hit (0, 46), hit (1000, 46) });
        CHECK (out.left[1000] == doctest::Approx (1.0f));
        CHECK (out.left[1000 + fadeSamples / 2] == doctest::Approx (0.5f + 0.5f * fade (fadeSamples / 2)));
        CHECK (out.left[1000 + fadeSamples] == 0.5f);
        CHECK (out.left[2999] == 0.5f);
    }

    SUBCASE ("pieces in no group ring over each other, and other groups are untouched")
    {
        DrumVoices voices;
        voices.setKit (kitOf (pieces));
        const auto out = render (voices, 3000, { hit (0, 49), hit (0, 36), hit (500, 49), hit (1000, 42) });
        CHECK (out.left[400] == 0.125f + 0.0625f);
        CHECK (out.left[2000] == 0.125f + 0.125f + 0.0625f + 0.25f);
    }

    SUBCASE ("a kick in a group of its own cuts itself")
    {
        DrumVoices voices;
        voices.setKit (kitOf (pieces));
        const auto out = render (voices, 3000, { hit (0, 36), hit (1000, 36) });
        CHECK (out.left[1000 + fadeSamples] == 0.0625f);
    }
}

TEST_CASE ("drum voices: silence fades every hit out over 10 ms, as an event or asked for by the message thread")
{
    const auto makeVoices = []
    {
        auto voices = std::make_unique<DrumVoices>();
        voices->setKit (kitOf ({ { 49, constant (0.5f, 10000), 1.0f, -1 }, { 36, constant (0.25f, 10000), 1.0f, -1 } }));
        return voices;
    };

    auto byEvent = makeVoices();
    const auto out = render (*byEvent, 3000, { hit (0, 49), hit (0, 36), { DrumEvent::Kind::silence, 1000, 0, 0 } });
    CHECK (out.left[999] == 0.75f);
    CHECK (out.left[1000] == 0.75f);
    CHECK (out.left[1000 + fadeSamples - 1] == doctest::Approx (0.75f / static_cast<float> (fadeSamples)));
    CHECK (out.left[1000 + fadeSamples] == 0.0f);

    auto asked = makeVoices();
    render (*asked, 1000, { hit (0, 49) });
    asked->requestSilence();
    const auto after = render (*asked, 1000, {});
    CHECK (after.left[0] == 0.5f);
    CHECK (after.left[fadeSamples] == 0.0f);

    // A hit after the silence plays as usual.
    const auto again = render (*asked, 100, { hit (10, 36) });
    CHECK (again.left[10] == 0.25f);
}

TEST_CASE ("drum voices: when hits pile up, the oldest fade out to make room, and none is dropped or cut short")
{
    DrumVoices voices;
    voices.setKit (kitOf ({ { 49, constant (0.01f, 44100), 1.0f, -1 } }));

    // A one-second crash struck every 100 samples (441 a second, several
    // times a fast roll): far more ringing hits than the pool holds.
    std::vector<DrumEvent> events;

    for (int i = 0; i < 1000; ++i)
        events.push_back (hit (i * 100, 49));

    render (voices, 101000, events, { 256 });
    CHECK (voices.counts().hits == 1000);
    CHECK (voices.counts().cutShort == 0);

    // Hits faster than fades can free voices for them cut some short; still none dropped.
    DrumVoices burst;
    burst.setKit (kitOf ({ { 49, constant (0.01f, 44100), 1.0f, -1 } }));
    render (burst, 1000, std::vector<DrumEvent> (100, hit (0, 49)));
    CHECK (burst.counts().hits == 100);
    CHECK (burst.counts().cutShort > 0);
}

TEST_CASE ("drum voices: a new kit plays from the next block, old hits ring on, and a kit is freed once nothing plays from it")
{
    DrumVoices voices;
    CHECK (voices.kitsHeld() == 0);

    // Nothing plays before the first kit.
    CHECK (render (voices, 10, { hit (0, 36) }).left[0] == 0.0f);

    voices.setKit (kitOf ({ { 36, constant (0.25f, 1000), 1.0f, 0 } }));
    voices.collectRetired();
    CHECK (voices.kitsHeld() == 1);
    render (voices, 100, { hit (0, 36) });

    // The new kit's kick is louder, and in the same choke group number: the
    // old kit's hit isn't choked by the new kit's.
    voices.setKit (kitOf ({ { 36, constant (0.5f, 1000), 1.0f, 0 } }));
    CHECK (voices.kitsHeld() == 2);

    const auto out = render (voices, 100, { hit (50, 36) });
    CHECK (out.left[0] == 0.25f);
    CHECK (out.left[50] == 0.75f);

    voices.collectRetired();
    CHECK (voices.kitsHeld() == 2); // the first kit's hit is still sounding

    render (voices, 1000, {});
    voices.collectRetired();
    CHECK (voices.kitsHeld() == 1);

    // A kit handed over and replaced before the audio thread took it is freed at once.
    voices.setKit (kitOf ({ { 36, constant (0.1f, 10), 1.0f, -1 } }));
    voices.setKit (kitOf ({ { 36, constant (0.2f, 10), 1.0f, -1 } }));
    CHECK (voices.kitsHeld() == 2);
    CHECK (render (voices, 10, { hit (0, 36) }).left[0] == 0.2f);
    voices.collectRetired();
    CHECK (voices.kitsHeld() == 1);

    // A silent kit plays nothing.
    voices.setKit (PlayableKit::silent (rate));
    CHECK (render (voices, 10, { hit (0, 36) }).left[0] == 0.0f);
}

TEST_CASE ("drum voices: nothing is allocated or freed on the audio thread")
{
    const auto makeKit = [] { return kitOf ({ { 42, constant (0.25f, 4000), 1.0f, 0 }, { 46, constant (0.5f, 9000), 1.0f, 0 }, { 49, constant (0.1f, 20000), 1.0f, -1 } }); };
    DrumVoices voices;
    voices.setKit (makeKit());

    std::vector<DrumEvent> events;
    std::mt19937 random (5);

    for (int i = 0; i < 400; ++i)
        events.push_back (hit (i * 20, std::array { 42, 46, 49, 50 }[static_cast<std::size_t> (std::uniform_int_distribution (0, 3) (random))],
                               std::uniform_int_distribution (0, 127) (random)));

    events.insert (events.end(), 100, hit (4000, 49)); // more than the pool at once
    events.push_back ({ DrumEvent::Kind::silence, 7000, 0, 0 });
    std::ranges::stable_sort (events, {}, &DrumEvent::offset);

    std::vector<float> left (512), right (512);
    std::vector<DrumEvent> blockEvents;
    blockEvents.reserve (events.size());
    std::size_t allocations = 0, frees = 0;

    for (int start = 0; start < 12000; start += 512)
    {
        blockEvents.clear();

        for (auto event : events)
            if (event.offset >= start && event.offset < start + 512)
                blockEvents.push_back ({ event.kind, event.offset - start, event.note, event.velocity });

        // A new kit to take, now and then, and silence asked for.
        if (start % 2048 == 0)
            voices.setKit (makeKit());

        if (start == 10240)
            voices.requestSilence();

        {
            const testing::CountAllocations count;
            voices.process (left.data(), right.data(), 512, blockEvents);
            allocations += count.allocations();
            frees += count.frees();
        }

        voices.collectRetired();
    }

    CHECK (voices.counts().hits > 300);
    CHECK (voices.counts().cutShort > 0);
    CHECK (allocations == 0);
    CHECK (frees == 0);

    // The counter itself works.
    const testing::CountAllocations count;
    auto made = std::make_unique<int> (1);
    made.reset();
    CHECK (count.allocations() == 1);
    CHECK (count.frees() == 1);
}
