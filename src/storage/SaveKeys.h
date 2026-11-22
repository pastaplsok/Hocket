#pragma once

// Saving from hocket-headless --play's console (SPEC §4.5): S saves, as the
// person at the console.
//
// A save refused because it would overwrite changes made outside Hocket says
// which, and offers O, which saves over them, keeping the versions on disk as
// a kept save first (Save::overwrite). O does that only straight after such a
// refusal, and only while the project is as it was then; never a second press
// of S (Lee, 2026-11-21). Any other key leaves the files as they are. O counts
// only a second or more after the refusal, so it isn't typed with the S, and it
// saves over only what the refusal named, as it was then (Save::overwriteOnly):
// if more has changed since, it's refused again (Lee, 2026-11-22).
//
// Message thread only, like the bus.

#include "core/commands/CommandBus.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace hocket::storage
{
    class SaveKeys
    {
    public:
        SaveKeys (commands::CommandBus& bus, commands::Author person);

        // What --play says at the start about its keys.
        static constexpr std::string_view hint = "S saves";

        using Clock = std::chrono::steady_clock;

        // A key typed in the console (upper or lower case), at `now`. Returns
        // what to print, if anything: keys that mean nothing here, with no
        // offer pending, say nothing.
        std::optional<std::string> press (char32_t key, Clock::time_point now = Clock::now());

        // How long after a refusal O starts to count: one sooner is typing
        // (a word starting "so"), not an answer to the refusal (Lee, 2026-11-22).
        static constexpr Clock::duration readingTime = std::chrono::seconds (1);

    private:
        commands::CommandBus& bus;
        commands::Author person;

        // A save refused for changes made outside Hocket: O saves over them
        // while the project is still at this revision, and the files are
        // still as the refusal found them (Save::overwriteOnly).
        struct Offer
        {
            commands::CommandBus::Revision revision = 0;
            Clock::time_point refusedAt;
            std::string outsideToken;
        };

        std::optional<Offer> offer;

        std::string save (bool overwrite, Clock::time_point now);
    };
}
