#include "core/kits/KitCatalog.h"

#include "core/Paths.h"
#include "core/Suggestions.h"
#include "core/kits/WavHeader.h"
#include "core/project/Rules.h"

#include <algorithm>
#include <system_error>

namespace hocket::kits
{
namespace
{
    // How many of a broken kit's errors its problem lists.
    constexpr std::size_t errorsShown = 3;

    std::string inQuotes (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    // The folders inside `folder`, by name; none if it isn't a folder.
    std::vector<std::filesystem::path> subfolders (const std::filesystem::path& folder)
    {
        std::vector<std::filesystem::path> found;
        std::error_code error;

        if (folder.empty() || ! std::filesystem::is_directory (folder, error))
            return found;

        for (std::filesystem::directory_iterator entry (folder, error), end; ! error && entry != end; entry.increment (error))
            if (std::error_code typeError; entry->is_directory (typeError))
                found.push_back (entry->path());

        std::ranges::sort (found);
        return found;
    }

    // Why the kit in `folder` can't be used, or empty if it can, with its
    // manifest, and the stamps of its files just before they were read.
    std::string readKit (const std::filesystem::path& folder, Manifest& manifest, std::vector<FileStamp>& stamps)
    {
        const auto manifestFile = folder / pathFromUtf8 (std::string (manifestFileName));
        std::vector<FileStamp> read { stampOf (manifestFile) };
        const auto text = readTextFile (manifestFile);

        if (! text)
            return "it has no " + std::string (manifestFileName) + ", the manifest that says which samples the kit's pieces play (SPEC §8.4)";

        auto parsed = parseManifest (*text);

        if (! parsed.manifest)
        {
            std::string why = std::string (manifestFileName) + " has " + (parsed.errors.size() == 1 ? "an error" : std::to_string (parsed.errors.size()) + " errors")
                            + ": ";

            for (std::size_t i = 0; i < std::min (parsed.errors.size(), errorsShown); ++i)
                why += (i > 0 ? "; " : "") + project::formatDiagnostic (parsed.errors[i]);

            if (parsed.errors.size() > errorsShown)
                why += "; and " + std::to_string (parsed.errors.size() - errorsShown) + " more";

            return why;
        }

        std::vector<std::string> missing;

        for (const auto& piece : parsed.manifest->pieces)
            if (std::error_code error; ! std::filesystem::is_regular_file (folder / pathFromUtf8 (piece.file), error))
                missing.push_back (inQuotes (piece.file) + " (piece " + inQuotes (piece.name) + ")");

        if (! missing.empty())
        {
            std::string list;

            for (const auto& file : missing)
                list += (list.empty() ? "" : ", ") + file;

            return (missing.size() == 1 ? "a sample it names isn't in its folder: " : "samples it names aren't in its folder: ") + list;
        }

        // What the samples' headers rule out. A header that can't be read is
        // left to the decoder, which decides when the kit loads.
        std::string unusable;

        for (const auto& piece : parsed.manifest->pieces)
        {
            read.push_back (stampOf (folder / pathFromUtf8 (piece.file)));

            if (const auto header = readWavHeader (folder / pathFromUtf8 (piece.file)))
                if (const auto problem = sampleFormatProblem (*header))
                    unusable += (unusable.empty() ? "" : "; ") + inQuotes (piece.file) + " (piece " + inQuotes (piece.name) + "): " + *problem;
        }

        if (! unusable.empty())
            return "a sample can't be played: " + unusable;

        manifest = std::move (*parsed.manifest);
        stamps = std::move (read);
        return {};
    }
}

//==============================================================================
KitCatalog::KitCatalog (std::filesystem::path bundledFolder, std::filesystem::path userFolder)
    : bundled (std::move (bundledFolder)), user (std::move (userFolder))
{
    scan();
}

void KitCatalog::add (Kit kit)
{
    kit.source = project::isUserKit (kit.name) ? Source::user : Source::bundled;
    broken.erase (kit.name);
    auto name = kit.name;
    usable.insert_or_assign (std::move (name), std::move (kit));
}

Lookup KitCatalog::find (std::string_view name)
{
    if (const auto found = usable.find (name); found != usable.end())
        return { &found->second, {} };

    scan();

    if (const auto found = usable.find (name); found != usable.end())
        return { &found->second, {} };

    if (const auto found = broken.find (name); found != broken.end())
        return { nullptr, "kit " + inQuotes (name) + " can't be used: " + found->second };

    return { nullptr, notInstalled (name) };
}

std::vector<const Kit*> KitCatalog::kits() const
{
    std::vector<const Kit*> list;

    for (const auto source : { Source::bundled, Source::user })
        for (const auto& [name, kit] : usable)
            if (kit.source == source)
                list.push_back (&kit);

    return list;
}

std::vector<std::string> KitCatalog::problems() const
{
    std::vector<std::string> list;

    for (const auto& [name, why] : broken)
        list.push_back ("kit " + inQuotes (name) + " can't be used: " + why);

    for (const auto& [folder, why] : misnamed)
        list.push_back (inQuotes (folder) + " isn't a kit Hocket can use: " + why);

    return list;
}

std::string KitCatalog::describeSource (const Kit& kit)
{
    if (kit.source == Source::bundled)
        return "bundled";

    return "the user's own, in " + utf8FromPath (kit.folder);
}

Lookup KitCatalog::refresh (std::string_view name)
{
    const auto found = usable.find (name);

    if (found == usable.end() || found->second.folder.empty())
        return find (name);

    auto& kit = found->second;
    Manifest manifest;
    std::vector<FileStamp> stamps;

    if (auto why = readKit (kit.folder, manifest, stamps); ! why.empty())
    {
        kit.problem = why;
    }
    else
    {
        kit.manifest = std::move (manifest);
        kit.stamps = std::move (stamps);
        kit.problem.clear();
    }

    return { &kit, {} };
}

void KitCatalog::setLoadReport (std::string_view name, LoadReport report)
{
    if (const auto found = usable.find (name); found != usable.end())
        found->second.loaded = std::move (report);
}

//==============================================================================
FileStamp stampOf (const std::filesystem::path& file)
{
    std::error_code error;
    const auto size = std::filesystem::file_size (file, error);

    if (error)
        return {};

    const auto modified = std::filesystem::last_write_time (file, error);

    if (error)
        return {};

    return { true, size, modified.time_since_epoch().count() };
}

std::vector<FileStamp> stampsOf (const Kit& kit)
{
    std::vector<FileStamp> stamps { stampOf (kit.folder / pathFromUtf8 (std::string (manifestFileName))) };

    for (const auto& piece : kit.manifest.pieces)
        stamps.push_back (stampOf (kit.folder / pathFromUtf8 (piece.file)));

    return stamps;
}

//==============================================================================
void KitCatalog::scan()
{
    broken.clear();
    misnamed.clear();
    scanFolder (bundled, Source::bundled);
    scanFolder (user, Source::user);
}

void KitCatalog::scanFolder (const std::filesystem::path& folder, Source source)
{
    for (const auto& kitFolder : subfolders (folder))
    {
        const auto folderName = utf8FromPath (kitFolder.filename());

        if (const auto problem = project::kitFolderNameProblem (folderName))
        {
            misnamed.insert_or_assign (utf8FromPath (kitFolder), *problem + "; rename the folder");
            continue;
        }

        const auto name = source == Source::user ? std::string (project::userKitPrefix) + folderName : folderName;

        // A kit already listed isn't read again: what's playing stays what was read.
        if (usable.contains (name))
            continue;

        Manifest manifest;
        std::vector<FileStamp> stamps;

        if (auto why = readKit (kitFolder, manifest, stamps); ! why.empty())
            broken.insert_or_assign (name, utf8FromPath (kitFolder) + ": " + why);
        else
            usable.insert_or_assign (name, Kit { name, source, kitFolder, std::move (manifest), {}, {}, std::move (stamps) });
    }
}

std::string KitCatalog::notInstalled (std::string_view name) const
{
    auto message = "kit " + inQuotes (name) + " isn't installed";

    // "dusty-909" for "user/dusty-909", or the other way round, or a typo.
    std::vector<std::string> nearest;

    for (const auto& [installed, kit] : usable)
        if (project::kitFolderName (installed) == project::kitFolderName (name) || editDistance (installed, name) <= 2)
            nearest.push_back (installed);

    if (nearest.size() == 1)
        message += " (did you mean " + inQuotes (nearest.front()) + "?)";

    if (usable.empty())
    {
        message += ". No kits are installed: bundled kits go in " + (bundled.empty() ? std::string ("kits\\ next to Hocket's programs") : utf8FromPath (bundled))
                 + ", and the user's own in " + (user.empty() ? std::string ("Documents\\Hocket\\Kits") : utf8FromPath (user))
                 + ", named \"user/<folder>\"";
        return message;
    }

    message += ". The installed kits are ";
    const auto list = kits();

    for (std::size_t i = 0; i < list.size(); ++i)
        message += (i > 0 ? ", " : "") + inQuotes (list[i]->name) + (list[i]->source == Source::bundled ? " (bundled)" : " (the user's own)");

    return message;
}
}
