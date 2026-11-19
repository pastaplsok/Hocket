#pragma once

// Made-up kits for the bus's tests: a kit as the catalog would list it, with
// no folder or samples, since the bus reads only a kit's manifest.

#include "core/kits/KitCatalog.h"

#include <string>
#include <utility>
#include <vector>

namespace hocket::testing
{
    // Pieces by name and note, each at 0 dB, in no choke group. A name
    // starting "user/" makes it one of the user's kits.
    inline kits::Kit testKit (std::string name, const std::vector<std::pair<std::string, int>>& pieces, std::string description = "Made up")
    {
        kits::Kit kit;
        kit.name = std::move (name);
        kit.manifest.description = std::move (description);

        for (const auto& [piece, note] : pieces)
            kit.manifest.pieces.push_back ({ piece, note, piece + ".wav", 0.0 });

        return kit;
    }
}
