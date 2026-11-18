#pragma once

// The kits Hocket can play (SPEC §8.4): the bundled ones, in kits\ next to
// the programs, and the user's own, in Documents\Hocket\Kits, which a project
// names with "user/" ("user/dusty-909"), so the two never clash. A kit is a
// folder holding kit.json and its samples.
//
// The folders are listed when the catalog is made, and again whenever a name
// isn't found, so a kit added during a session can be used without a
// restart. A kit already listed isn't read again.
//
// Message thread only, like the bus.

#include "core/kits/Kit.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::kits
{
    enum class Source
    {
        bundled, // installed with Hocket
        user     // in the user's folder: named "user/<folder>"
    };

    struct Kit
    {
        std::string name; // as a project names it: "acoustic", "user/dusty-909"
        Source source = Source::bundled;
        std::filesystem::path folder; // holding kit.json and the samples
        Manifest manifest;
    };

    // What looking a kit up found: the kit, or why there's none.
    struct Lookup
    {
        const Kit* kit = nullptr;
        std::string problem; // without a kit: it isn't installed (and what is), or it's broken (and why)
    };

    class KitCatalog
    {
    public:
        // No folders: kits come only from add(), for tests.
        KitCatalog() = default;

        // Lists the kits in both folders. Either may not exist, or be empty
        // (when the OS doesn't say where Documents is).
        KitCatalog (std::filesystem::path bundledFolder, std::filesystem::path userFolder);

        // Adds a kit as if it had been found in a folder (for tests). Its name
        // says where: "user/..." for a user kit.
        void add (Kit kit);

        // The kit with this name, as a project names it. When there's none,
        // the folders are listed again first.
        Lookup find (std::string_view name);

        // The usable kits: the bundled ones first, then the user's, each by name.
        std::vector<const Kit*> kits() const;

        // Folders in the kit folders that hold no usable kit, and why, for
        // --play to say at start: each a line naming the folder.
        std::vector<std::string> problems() const;

        // Where the kit came from, for the overview: "bundled", or "the user's
        // own, in <folder>".
        static std::string describeSource (const Kit& kit);

        const std::filesystem::path& bundledFolder() const { return bundled; }
        const std::filesystem::path& userFolder() const { return user; }

    private:
        std::filesystem::path bundled;
        std::filesystem::path user;
        std::map<std::string, Kit, std::less<>> usable;           // by name
        std::map<std::string, std::string, std::less<>> broken;   // by name: why it can't be used
        std::map<std::string, std::string, std::less<>> misnamed; // by folder, as UTF-8: folders whose names aren't kit names

        void scan();
        void scanFolder (const std::filesystem::path& folder, Source source);
        std::string notInstalled (std::string_view name) const;
    };
}
