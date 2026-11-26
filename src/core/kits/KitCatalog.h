#pragma once

// The kits Hocket can play (SPEC §8.4): the bundled ones, in kits\ next to
// the programs, and the user's own, in Documents\Hocket\Kits, which a project
// names with "user/" ("user/dusty-909"), so the two never clash. A kit is a
// folder holding kit.json and its samples.
//
// The folders are listed when the catalog is made, and again whenever a name
// isn't found, so a kit added during a session can be used without a
// restart. A kit already listed is read again only by refresh(), when the
// engine sees its files change (a watched user kit).
//
// Message thread only, like the bus.

#include "core/kits/Kit.h"

#include <cstdint>
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

    // What loading a kit's samples found that their headers couldn't say:
    // samples the decoder refused, and pieces that peak above full scale (a
    // 32-bit float sample can).
    struct LoadReport
    {
        struct Loud
        {
            std::string piece;
            double peak = 0.0; // dBFS, above 0

            bool operator== (const Loud&) const = default;
        };

        std::vector<std::string> problems;
        std::vector<Loud> overFullScale;

        // Why the kit couldn't be loaded at all, so it plays nothing, as if
        // it were missing; empty when it loaded.
        std::string failed;

        bool operator== (const LoadReport&) const = default;
    };

    // A file's size and modified time, to tell when it changes (SPEC §8.4).
    struct FileStamp
    {
        bool exists = false;
        std::uintmax_t size = 0;
        std::int64_t modified = 0; // in the file system's own ticks

        bool operator== (const FileStamp&) const = default;
    };

    struct Kit
    {
        std::string name; // as a project names it: "acoustic", "user/dusty-909"
        Source source = Source::bundled;
        std::filesystem::path folder; // holding kit.json and the samples
        Manifest manifest;

        // Why the kit's files as they are now can't be used, when they've
        // changed since it was read: this manifest, the last that read, plays on.
        std::string problem;

        LoadReport loaded; // from the latest load of its samples

        // The stamps of kit.json and each sample (as stampsOf orders them),
        // each taken just before the manifest read it: a change made since,
        // even before the kit was first used, shows as a difference. Empty
        // for a kit add()ed.
        std::vector<FileStamp> stamps;
    };

    FileStamp stampOf (const std::filesystem::path& file);

    // Stamps of the kit's kit.json and each of its samples, in order: when
    // they change, the kit's files have.
    std::vector<FileStamp> stampsOf (const Kit& kit);

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

        // Reads a listed kit from its folder again, when its files have
        // changed (watched user kits, SPEC §8.4). If it can't be used as it is
        // now, the manifest it had stays, with the reason in Kit::problem. A
        // kit not listed is looked up as find() would.
        Lookup refresh (std::string_view name);

        // What the latest load of the kit's samples found, for the overview.
        void setLoadReport (std::string_view name, LoadReport report);

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
