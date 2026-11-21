#pragma once

// Text that names things on disk, shared by saving and the recovery copy:
// folder names Windows takes whatever they're made from, and times as
// .hocket/save.json and .hocket/recovery.json write them.

#include "core/commands/Storage.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace hocket::storage
{
    // A name Windows takes as it is: characters Windows refuses in a name
    // (<>:"/\|?* and control characters) become spaces, runs of spaces one,
    // invalid UTF-8 is dropped, it's cut short to `maxUnits` UTF-16 code units,
    // and trailing dots and spaces go. It can still be a reserved name like
    // CON, so callers start it with something that isn't.
    std::string windowsSafeName (std::string_view text, std::size_t maxUnits);

    // "2026-11-20T13:02:37Z": UTC, to the second.
    std::string utcTimeText (commands::SaveTime time);

    // A time exactly as utcTimeText writes it; nullopt for anything else.
    std::optional<commands::SaveTime> parseUtcTime (std::string_view text);
}
