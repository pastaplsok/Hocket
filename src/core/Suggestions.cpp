#include "core/Suggestions.h"

#include <algorithm>
#include <vector>

namespace hocket
{
std::size_t editDistance (std::string_view a, std::string_view b)
{
    std::vector<std::size_t> row (b.size() + 1);

    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;

    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        auto diagonal = row[0];
        row[0] = i;

        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const auto above = row[j];
            row[j] = std::min ({ row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1) });
            diagonal = above;
        }
    }

    return row[b.size()];
}
}
