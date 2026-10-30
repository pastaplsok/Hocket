#pragma once

// How a clip's stored .notes text differs from the text a client sent. The bus
// stores what the text parses to and writes it back in canonical form (SPEC
// §5.4), so write_clip tells the client when its own copy no longer matches.

#include <string>
#include <string_view>

namespace hocket::api
{
    // A short summary for an LLM reader, e.g. "rewrote 2 lines as one:
    // `1.1 C3 1/2 v80`, `1.1 E3 1/2 v80` became `1.1      [C3 E3]  1/2   v80`;
    // changed spacing". Empty when the texts are the same.
    std::string describeChanges (std::string_view sent, std::string_view stored);
}
