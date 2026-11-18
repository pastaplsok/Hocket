#include "engine/devices/DrumVoices.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hocket::engine
{
DrumVoices::DrumVoices() = default;

// Nothing plays any more: Tracktion frees a plugin only once no graph holds it.
DrumVoices::~DrumVoices() = default;

//==============================================================================
void DrumVoices::setKit (std::unique_ptr<PlayableKit> kit)
{
    if (kit == nullptr)
        return;

    kit->generation = nextGeneration++;
    auto* handedOver = kit.get();
    owned.push_back (std::move (kit));

    // The audio thread takes the pending kit by exchange too, so whichever of
    // us gets a kit out of `pending` is the only one that ever had it: one
    // replaced here was never seen by the audio thread.
    if (auto* replaced = pending.exchange (handedOver, std::memory_order_acq_rel))
        std::erase_if (owned, [replaced] (const auto& held) { return held.get() == replaced; });

    collectRetired();
}

void DrumVoices::collectRetired()
{
    // Every kit the audio thread could still read is at least this new: its
    // current kit, the kits its hits play from, and any kit not yet taken.
    const auto oldest = oldestInUse.load (std::memory_order_acquire);
    std::erase_if (owned, [oldest] (const auto& held) { return held->generation < oldest; });
}

void DrumVoices::requestSilence() noexcept
{
    silenceRequested.store (true, std::memory_order_release);
}

//==============================================================================
void DrumVoices::process (float* left, float* right, int numSamples, std::span<const DrumEvent> events) noexcept
{
    if (auto* kit = pending.exchange (nullptr, std::memory_order_acq_rel))
        current = kit;

    if (silenceRequested.exchange (false, std::memory_order_acq_rel))
        for (auto& voice : voices)
            fadeOut (voice);

    int done = 0;

    for (const auto& event : events)
    {
        const auto at = std::clamp (event.offset, done, numSamples);
        render (left, right, done, at);
        done = at;

        if (event.kind == DrumEvent::Kind::silence)
        {
            for (auto& voice : voices)
                fadeOut (voice);
        }
        else if (event.velocity > 0) // velocity 0 is a note-off: one-shots ignore it
        {
            startHit (event);
        }
    }

    render (left, right, done, numSamples);
    publishOldestInUse();
}

void DrumVoices::startHit (const DrumEvent& event) noexcept
{
    if (current == nullptr)
        return;

    const auto* piece = current->pieceOn (event.note);

    if (piece == nullptr || piece->length == 0)
        return;

    // A choke group cuts every sounding hit in it, the piece's own included (SPEC §5.2).
    if (piece->chokeGroup >= 0)
        for (auto& voice : voices)
            if (voice.piece != nullptr && voice.generation == current->generation && voice.piece->chokeGroup == piece->chokeGroup)
                fadeOut (voice);

    // A free voice; or, if more hits than the pool holds start within one
    // fade, the one nearest silence.
    Voice* chosen = nullptr;

    for (auto& voice : voices)
    {
        if (voice.piece == nullptr)
        {
            chosen = &voice;
            break;
        }

        if (chosen == nullptr || voice.level() < chosen->level() || (voice.level() == chosen->level() && voice.started < chosen->started))
            chosen = &voice;
    }

    if (chosen->piece != nullptr)
        ++audioCounts.cutShort;

    *chosen = { piece, current->generation, nextStart++, 0, piece->gain * velocityGain (event.velocity), 0, 0 };
    ++audioCounts.hits;

    // Keep some voices free: the oldest hit still at full level fades out.
    const auto free = static_cast<std::size_t> (std::ranges::count_if (voices, [] (const Voice& voice) { return voice.piece == nullptr; }));

    if (free < voicesKeptFree)
    {
        Voice* oldest = nullptr;

        for (auto& voice : voices)
            if (voice.piece != nullptr && voice.fadeLength == 0 && &voice != chosen && (oldest == nullptr || voice.started < oldest->started))
                oldest = &voice;

        if (oldest != nullptr)
            fadeOut (*oldest);
    }
}

void DrumVoices::fadeOut (Voice& voice) const noexcept
{
    if (voice.piece == nullptr || voice.fadeLength != 0 || current == nullptr)
        return;

    voice.fadeLength = static_cast<std::uint32_t> (std::max (1.0, std::round (fadeSeconds * current->sampleRate())));
    voice.fadeLeft = voice.fadeLength;
}

void DrumVoices::render (float* left, float* right, int from, int to) noexcept
{
    if (to <= from)
        return;

    for (auto& voice : voices)
    {
        if (voice.piece == nullptr)
            continue;

        const auto& piece = *voice.piece;
        const auto count = std::min (static_cast<std::size_t> (to - from), piece.length - voice.position);
        const auto* sourceLeft = piece.left + voice.position;
        const auto* sourceRight = piece.right + voice.position;
        auto* destLeft = left + from;
        auto* destRight = right + from;

        if (voice.fadeLength == 0)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                destLeft[i] += sourceLeft[i] * voice.gain;
                destRight[i] += sourceRight[i] * voice.gain;
            }

            voice.position += count;
        }
        else
        {
            // Linear in gain, to silence: the first sample at full level, the
            // last at 1 / fadeLength, then none.
            const auto length = static_cast<float> (voice.fadeLength);
            std::size_t i = 0;

            for (; i < count && voice.fadeLeft > 0; ++i, --voice.fadeLeft)
            {
                const auto gain = voice.gain * (static_cast<float> (voice.fadeLeft) / length);
                destLeft[i] += sourceLeft[i] * gain;
                destRight[i] += sourceRight[i] * gain;
            }

            voice.position += i;

            if (voice.fadeLeft == 0)
            {
                voice.piece = nullptr;
                continue;
            }
        }

        if (voice.position >= piece.length)
            voice.piece = nullptr;
    }
}

void DrumVoices::publishOldestInUse() noexcept
{
    if (current == nullptr)
        return; // 0 stays: nothing is freed but kits never taken

    auto oldest = current->generation;

    for (const auto& voice : voices)
        if (voice.piece != nullptr)
            oldest = std::min (oldest, voice.generation);

    oldestInUse.store (oldest, std::memory_order_release);
}
}
