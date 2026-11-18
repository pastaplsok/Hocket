#include "engine/devices/PlayableKit.h"

#include <stdexcept>
#include <string>

namespace hocket::engine
{
PlayableKit::PlayableKit (double sampleRate, std::vector<Piece> kitPieces)
    : rate (sampleRate), pieces (std::move (kitPieces))
{
    if (! (rate > 0.0))
        throw std::invalid_argument ("a kit's sample rate must be above zero");

    byNote.fill (-1);
    voicings.reserve (pieces.size());

    for (const auto& piece : pieces)
    {
        if (piece.note < 0 || piece.note > 127)
            throw std::invalid_argument ("a kit's piece is on note " + std::to_string (piece.note) + ", outside 0 to 127");

        if (byNote[static_cast<std::size_t> (piece.note)] >= 0)
            throw std::invalid_argument ("a kit has two pieces on note " + std::to_string (piece.note));

        if (piece.audio == nullptr || piece.audio->channels.empty() || piece.audio->channels.size() > 2)
            throw std::invalid_argument ("the piece on note " + std::to_string (piece.note) + " should have one or two channels of audio");

        const auto& channels = piece.audio->channels;

        if (channels.size() == 2 && channels[0].size() != channels[1].size())
            throw std::invalid_argument ("the piece on note " + std::to_string (piece.note) + " has channels of different lengths");

        byNote[static_cast<std::size_t> (piece.note)] = static_cast<int> (voicings.size());
        voicings.push_back ({ channels.front().data(), channels.back().data(), channels.front().size(), piece.gain, piece.chokeGroup });
    }
}

std::unique_ptr<PlayableKit> PlayableKit::silent (double sampleRate)
{
    return std::make_unique<PlayableKit> (sampleRate, std::vector<Piece> {});
}
}
