#pragma once

// Storage for the command bus that keeps everything in memory: what each save
// was asked to save, and kept saves a test puts there.

#include "core/commands/Storage.h"

#include <chrono>
#include <string>
#include <vector>

namespace hocket::testing
{
    // Storage that keeps each project it's asked to save, or fails when told
    // to, with kept saves a test puts there.
    struct FakeStorage final : commands::Storage
    {
        commands::SaveResult save (const project::Project& project, const commands::Save& request, const commands::Author& author) override
        {
            if (fail)
                return { false, "didn't save: the disk is full" };

            saves.push_back ({ project, request.name, author });
            return { true, "saved" };
        }

        std::vector<commands::KeptSave> keptSaves() override
        {
            std::vector<commands::KeptSave> list;

            for (const auto& save : kept)
                list.push_back (save.info);

            return list;
        }

        project::LoadResult loadKeptSave (const commands::KeptSave& save) override
        {
            for (const auto& entry : kept)
                if (entry.info == save)
                    return entry.loaded;

            return { {}, { { {}, 0, 0, "no such kept save" } } };
        }

        // Kept as number, by Lee, at a fixed time.
        void keep (int number, project::Project project, std::string name = {})
        {
            const commands::KeptSave info { number, { commands::SaveTime { std::chrono::seconds (1'800'000'000) }, "Lee", std::move (name) },
                                            ".hocket/saves/" + std::to_string (number) };
            kept.push_back ({ info, { std::move (project), {} } });
        }

        struct Save
        {
            project::Project project;
            std::string name;
            commands::Author author;
        };

        struct Kept
        {
            commands::KeptSave info;
            project::LoadResult loaded;
        };

        std::vector<Save> saves;
        std::vector<Kept> kept;
        bool fail = false;
    };
}
