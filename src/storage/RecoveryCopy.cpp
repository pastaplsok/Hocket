#include "storage/RecoveryCopy.h"

#include "core/Paths.h"
#include "platform/Platform.h"
#include "storage/Formats.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>

namespace hocket::storage
{
namespace
{
    // What a write or remove returns when the step hook stopped it, as a crash would.
    constexpr std::string_view stopped = "stopped part-way by the step hook";

    std::string withoutStop (std::string text)
    {
        while (! text.empty() && text.back() == '.')
            text.pop_back();

        return text;
    }

    std::string quoted (const std::filesystem::path& path)
    {
        return "'" + utf8FromPath (path) + "'";
    }

    // FNV-1a, 64 bits: the same on every machine and in every build, as a
    // name on disk must be.
    std::uint64_t hashOf (std::string_view text)
    {
        std::uint64_t hash = 0xcbf29ce484222325ull;

        for (const auto c : text)
        {
            hash ^= static_cast<unsigned char> (c);
            hash *= 0x100000001b3ull;
        }

        return hash;
    }

    // "7" is a copy; "writing 8", "removing 7" and anything else aren't.
    std::optional<int> copyNumber (const std::string& name)
    {
        int number = 0;
        const auto* end = name.data() + name.size();
        const auto [next, error] = std::from_chars (name.data(), end, number);
        return error == std::errc() && next == end && number > 0 && name.front() != '0' ? std::optional<int> (number) : std::nullopt;
    }

    //==============================================================================
    // .hocket/recovery.json:
    //
    //   {
    //     "format": "hocket-recovery",
    //     "project": "D:\\Music\\my song",
    //     "written": "2026-11-21T14:02:37Z"
    //   }
    //
    // The project is its folder's full path; the time is UTC.

    struct RecoveryInfo
    {
        std::string project;
        commands::SaveTime written;
    };

    std::string recoveryInfoText (const RecoveryInfo& info)
    {
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        json["format"] = "hocket-recovery";
        json["project"] = info.project;
        json["written"] = utcTimeText (info.written);
        return json.dump (2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    }

    std::optional<RecoveryInfo> parseRecoveryInfo (std::string_view text)
    {
        const auto json = nlohmann::json::parse (text.begin(), text.end(), nullptr, false);

        if (json.is_discarded() || ! json.is_object() || ! json.contains ("project") || ! json.at ("project").is_string() || ! json.contains ("written")
            || ! json.at ("written").is_string())
            return {};

        const auto written = parseUtcTime (json.at ("written").get<std::string>());

        if (! written)
            return {};

        return RecoveryInfo { json.at ("project").get<std::string>(), *written };
    }

    std::optional<RecoveryInfo> recoveryInfoIn (const std::filesystem::path& copy)
    {
        const auto text = readTextFile (copy / pathFromUtf8 (recoveryInfoFile));
        return text ? parseRecoveryInfo (*text) : std::nullopt;
    }

    // The folder's full path, the same however it's written: canonical, which
    // on Windows also gives each name as the disk has it, whatever case it
    // was typed in. A folder that isn't there (it should be) is made absolute.
    std::filesystem::path fullPathOf (const std::filesystem::path& folder)
    {
        std::error_code error;
        auto full = std::filesystem::canonical (folder, error);

        if (error)
            full = std::filesystem::absolute (folder, error).lexically_normal();

        // "D:\Music\song\" names the same folder as "D:\Music\song".
        if (! full.has_filename() && full.has_relative_path())
            full = full.parent_path();

        return full;
    }

    // "my song 3f2a9c81d4e6b7a0": the folder's name, made safe for Windows
    // and cut short, then a hash of its full path.
    std::string placeName (const std::filesystem::path& fullPath)
    {
        auto name = windowsSafeName (utf8FromPath (fullPath.filename()), maxRecoveryNameLength);

        if (name.empty())
            name = "project"; // a drive's root, say

        return name + std::format (" {:016x}", hashOf (utf8FromPath (fullPath)));
    }
}

std::optional<std::filesystem::path> defaultRecoveryFolder()
{
    const auto appData = platform::localAppDataFolder();

    if (! appData)
        return {};

    return *appData / pathFromUtf8 ("Hocket") / pathFromUtf8 ("Recovery");
}

RecoveryCopy::RecoveryCopy (std::filesystem::path recoveryFolder, const std::filesystem::path& projectFolder, Clock writeClock)
    : clock (writeClock ? std::move (writeClock) : Clock ([] { return std::chrono::system_clock::now(); }))
{
    const auto fullPath = fullPathOf (projectFolder);
    projectPath = utf8FromPath (fullPath);
    placePath = std::move (recoveryFolder) / pathFromUtf8 (placeName (fullPath));
}

bool RecoveryCopy::step (Step::Kind kind, const std::filesystem::path& path) const
{
    return ! stepHook || stepHook (Step { kind, path });
}

std::vector<int> RecoveryCopy::copyNumbers() const
{
    std::vector<int> numbers;
    std::error_code error;

    for (std::filesystem::directory_iterator entry (placePath, error), end; ! error && entry != end; entry.increment (error))
        if (const auto number = copyNumber (utf8FromPath (entry->path().filename())); number && entry->is_directory (error))
            numbers.push_back (*number);

    std::ranges::sort (numbers, std::greater<>());
    return numbers;
}

std::optional<RecoveryCopy::Found> RecoveryCopy::find() const
{
    // A copy whose recovery.json doesn't read isn't one Hocket wrote whole,
    // so the next one down is tried.
    for (const auto number : copyNumbers())
    {
        const auto folder = placePath / pathFromUtf8 (std::to_string (number));

        if (const auto info = recoveryInfoIn (folder))
            return info->project == projectPath ? std::optional<Found> (Found { folder, info->written }) : std::nullopt;
    }

    return {};
}

std::optional<std::string> RecoveryCopy::otherProject() const
{
    for (const auto number : copyNumbers())
        if (const auto info = recoveryInfoIn (placePath / pathFromUtf8 (std::to_string (number))))
            return info->project != projectPath ? std::optional<std::string> (info->project) : std::nullopt;

    return {};
}

bool RecoveryCopy::clearLeftovers() const
{
    std::vector<std::filesystem::path> leftovers;
    std::error_code error;

    for (std::filesystem::directory_iterator entry (placePath, error), end; ! error && entry != end; entry.increment (error))
        if (const auto name = utf8FromPath (entry->path().filename()); name.starts_with ("writing ") || name.starts_with ("removing "))
            leftovers.push_back (entry->path());

    for (const auto& leftover : leftovers)
    {
        if (! step (Step::Kind::clearLeftover, leftover))
            return false;

        std::filesystem::remove_all (leftover, error); // if it stays, the next write tries again
    }

    return true;
}

std::string RecoveryCopy::takeAway (int number) const
{
    const auto copy = placePath / pathFromUtf8 (std::to_string (number));
    const auto removing = placePath / pathFromUtf8 ("removing " + std::to_string (number));
    std::error_code error;

    // One a crash left part-deleted, in the way of the rename.
    if (std::filesystem::exists (removing, error))
    {
        if (! step (Step::Kind::clearLeftover, removing))
            return std::string (stopped);

        std::filesystem::remove_all (removing, error);
    }

    if (! step (Step::Kind::takeAway, copy))
        return std::string (stopped);

    if (const auto moved = platform::moveFile (copy, removing); ! moved.empty())
        return "couldn't take away the recovery copy " + quoted (copy) + ": " + withoutStop (moved);

    if (! step (Step::Kind::deleteTakenAway, removing))
        return std::string (stopped);

    std::filesystem::remove_all (removing, error); // what's left is cleared by the next write
    return {};
}

std::string RecoveryCopy::write (const std::map<std::string, std::string>& files)
{
    if (const auto other = otherProject())
        return "the place for this project's recovery copy, " + quoted (placePath) + ", holds a copy of another project, " + *other
             + ", which Hocket won't write over";

    std::error_code error;
    std::filesystem::create_directories (placePath, error);

    if (error)
        return "couldn't make the folder " + quoted (placePath) + ": " + withoutStop (error.message());

    if (! clearLeftovers())
        return std::string (stopped);

    const auto older = copyNumbers();
    const auto number = older.empty() ? 1 : older.front() + 1;
    const auto writing = placePath / pathFromUtf8 ("writing " + std::to_string (number));
    const auto finished = placePath / pathFromUtf8 (std::to_string (number));

    const auto writeOne = [this, &writing] (const std::string& relative, std::string_view text) -> std::string
    {
        const auto path = writing / pathFromUtf8 (relative);

        if (! step (Step::Kind::writeFile, path))
            return std::string (stopped);

        std::error_code folderError;
        std::filesystem::create_directories (path.parent_path(), folderError);
        const auto problem = folderError ? folderError.message() : platform::writeFileToDisk (path, text);
        return problem.empty() ? std::string() : "couldn't write its " + relative + ": " + withoutStop (problem);
    };

    // Which project it's of and when, last: a copy is whole once it's there.
    const RecoveryInfo info { projectPath, std::chrono::floor<std::chrono::seconds> (clock()) };
    std::string problem;

    for (const auto& [relative, text] : files)
        if (problem = writeOne (relative, text); ! problem.empty())
            break;

    if (problem.empty())
        problem = writeOne (std::string (recoveryInfoFile), recoveryInfoText (info));

    if (problem.empty())
    {
        if (! step (Step::Kind::commit, finished))
            return std::string (stopped);

        if (const auto moved = platform::moveFile (writing, finished); ! moved.empty())
            problem = "couldn't rename " + quoted (writing) + " to " + quoted (finished) + ": " + withoutStop (moved);
    }

    if (problem == stopped)
        return problem;

    if (! problem.empty())
    {
        std::filesystem::remove_all (writing, error);
        return "couldn't write the recovery copy in " + quoted (placePath) + ": " + problem;
    }

    // The new copy is whole, so the ones before can go. One that can't is
    // taken away by the next write, and meanwhile find() takes the newest.
    for (const auto old : older)
        if (takeAway (old) == stopped)
            return std::string (stopped);

    return {};
}

std::string RecoveryCopy::remove()
{
    std::error_code error;

    if (! std::filesystem::exists (placePath, error) || otherProject())
        return {};

    std::string problem;

    for (const auto number : copyNumbers())
    {
        const auto taken = takeAway (number);

        if (taken == stopped)
            return taken;

        if (problem.empty())
            problem = taken;
    }

    if (! clearLeftovers())
        return std::string (stopped);

    std::filesystem::remove (placePath, error); // only once it's empty
    return problem;
}
}
