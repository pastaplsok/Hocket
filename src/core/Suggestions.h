#pragma once

// "Did you mean" for error messages.

#include <cstddef>
#include <string_view>

namespace hocket
{
    // How many single-byte insertions, deletions and substitutions turn a into b.
    std::size_t editDistance (std::string_view a, std::string_view b);
}
