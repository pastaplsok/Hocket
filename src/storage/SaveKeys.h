#pragma once

// Saving from hocket-headless --play's console (SPEC §4.5): S saves, as the
// person at the console.
//
// A save refused because it would overwrite changes made outside Hocket says
// which, and offers O, which saves over them, keeping the versions on disk as
// a kept save first (Save::overwrite). O does that only straight after such a
// refusal, and only while the project is as it was then; never a second press
// of S (Lee, 2026-11-21). Any other key leaves the files as they are.
//
// Message thread only, like the bus.

#include "core/commands/CommandBus.h"

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

        // A key typed in the console (upper or lower case). Returns what to
        // print, if anything: keys that mean nothing here, with no offer
        // pending, say nothing.
        std::optional<std::string> press (char32_t key);

    private:
        commands::CommandBus& bus;
        commands::Author person;

        // The project's revision when a save was refused for changes made
        // outside Hocket: O saves over them while it's still the same.
        std::optional<commands::CommandBus::Revision> offer;

        std::string save (bool overwrite);
    };
}
