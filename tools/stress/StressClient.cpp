#include "stress/StressClient.h"

#include "api/Protocol.h"
#include "api/TokenFile.h"

#include <algorithm>
#include <sstream>
#include <thread>

namespace hocket::stress
{
namespace
{
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;

    constexpr auto connectTimeout = std::chrono::seconds (5);
    constexpr std::size_t maxFailuresKept = 5;

    double secondsBetween (Clock::time_point from, Clock::time_point to)
    {
        return std::chrono::duration<double> (to - from).count();
    }

    std::string firstLine (const std::string& text)
    {
        return text.substr (0, text.find ('\n'));
    }

    std::string errorMessage (const Json& response)
    {
        const auto& error = response.at ("error");
        return error.is_object() && error.contains ("message") && error.at ("message").is_string() ? error.at ("message").get<std::string>()
                                                                                                    : error.dump();
    }

    struct ToolResult
    {
        bool ok = false;                 // answered, and not an error
        std::vector<std::string> blocks; // the result's text blocks; when not ok, what went wrong
    };

    ToolResult callTool (api::Client& client, const std::string& name, const Json& arguments, std::chrono::milliseconds timeout)
    {
        const auto response = client.request ("tools/call", { { "name", name }, { "arguments", arguments } }, timeout);

        if (response.status == api::Client::Status::closed)
            return { false, { "Hocket closed the connection" } };

        if (response.status == api::Client::Status::timedOut)
            return { false, { "no answer within " + std::to_string (timeout.count()) + " ms" } };

        if (response.message.contains ("error"))
            return { false, { errorMessage (response.message) } };

        ToolResult result;
        const auto& answer = response.message.at ("result");

        for (const auto& block : answer.value ("content", Json::array()))
            if (block.is_object() && block.contains ("text") && block.at ("text").is_string())
                result.blocks.push_back (block.at ("text").get<std::string>());

        result.ok = ! answer.value ("isError", false);

        if (result.blocks.empty())
            result.blocks.emplace_back();

        return result;
    }

    double percentile (std::vector<double> values, double fraction)
    {
        if (values.empty())
            return 0.0;

        std::ranges::sort (values);
        return values[static_cast<std::size_t> (fraction * static_cast<double> (values.size() - 1) + 0.5)];
    }
}

//==============================================================================
std::string connect (api::Client& client, const std::filesystem::path& tokenFile, const std::string& name, const std::string& session)
{
    const auto info = api::runningInstance (tokenFile);

    if (! info)
        return "Hocket isn't running: start it with hocket-headless --play <project folder>, then try again.";

    if (const auto error = client.connect (info->port, connectTimeout); ! error.empty())
        return "couldn't connect to Hocket on port " + std::to_string (info->port) + ": " + error;

    const Json hello { { "token", info->token },
                       { "version", api::hocketVersion() },
                       { "fingerprint", api::buildFingerprint() },
                       { "client", name },
                       { "session", session } };

    const auto response = client.request ("hello", hello, connectTimeout);

    if (response.status != api::Client::Status::answered)
        return "Hocket didn't answer hello";

    if (response.message.contains ("error"))
        return "Hocket refused the connection: " + errorMessage (response.message);

    return {};
}

std::string rewriteText (const notes::Clip& original, int index, int count, bool vary)
{
    auto clip = original;
    clip.header.insert (clip.header.begin(), " hocket-stress: rewrite " + std::to_string (index + 1) + " of " + std::to_string (count));

    if (vary && index % 2 == 1)
        for (auto& note : clip.notes)
            note.pitch += note.pitch + 12 <= 127 ? 12 : -12;

    return notes::write (clip);
}

Report run (api::Client& client, const Options& options)
{
    Report report;
    const auto read = callTool (client, "read_clip", { { "clip_id", options.clipId } }, options.replyTimeout);

    // read_clip's second block is the clip's text, exactly as stored.
    if (! read.ok || read.blocks.size() < 2)
    {
        report.error = "couldn't read clip " + options.clipId + ": " + read.blocks.front();
        return report;
    }

    const auto& originalText = read.blocks[1];
    const auto original = notes::parse (originalText);

    if (! original.clip)
    {
        report.error = "couldn't parse clip " + options.clipId + "'s text: " + notes::formatDiagnostic (original.errors.front(), options.clipId);
        return report;
    }

    const auto interval = std::chrono::duration<double> (1.0 / options.perSecond);
    const auto start = Clock::now();
    auto lastAnswer = start;

    for (int i = 0; i < options.count; ++i)
    {
        const auto due = start + std::chrono::duration_cast<Clock::duration> (i * interval);
        std::this_thread::sleep_until (due);

        const auto sentAt = Clock::now();
        report.mostBehind = std::max (report.mostBehind, secondsBetween (due, sentAt));

        const auto text = rewriteText (*original.clip, i, options.count, options.vary);
        const auto result = callTool (client, "write_clip", { { "clip_id", options.clipId }, { "notes", text } }, options.replyTimeout);

        lastAnswer = Clock::now();
        ++report.sent;
        report.replySeconds.push_back (secondsBetween (sentAt, lastAnswer));

        // A change's result names its history entry: "#12 wrote clip ...".
        if (result.ok && result.blocks.front().starts_with ("#"))
            ++report.applied;
        else if (report.failures.size() < maxFailuresKept)
            report.failures.push_back ("rewrite " + std::to_string (i + 1) + ": " + firstLine (result.blocks.front()));

        if (! client.isOpen())
        {
            report.error = "Hocket closed the connection after " + std::to_string (report.sent) + " rewrites";
            break;
        }
    }

    report.seconds = secondsBetween (start, lastAnswer);

    if (client.isOpen())
        report.restored = callTool (client, "write_clip", { { "clip_id", options.clipId }, { "notes", originalText } }, options.replyTimeout).ok;

    return report;
}

std::string summary (const Report& report, const Options& options)
{
    std::ostringstream text;
    text.setf (std::ios::fixed);
    text.precision (1);

    if (! report.error.empty() && report.sent == 0)
        return report.error + "\n";

    text << "Rewrote " << options.clipId << " " << report.sent << " times in " << report.seconds << " s (" << options.perSecond << " a second asked for).\n";

    if (report.applied == report.sent)
        text << "All " << report.applied << " were applied, each a history entry.\n";
    else
        text << report.applied << " of " << report.sent << " were applied. The first that weren't:\n";

    for (const auto& failure : report.failures)
        text << "  " << failure << "\n";

    text.precision (2);
    text << "Answers took " << percentile (report.replySeconds, 0.5) * 1000.0 << " ms (median), " << percentile (report.replySeconds, 0.95) * 1000.0
         << " ms (95th percentile), " << percentile (report.replySeconds, 1.0) * 1000.0 << " ms at most.\n";

    // A send is late when the answer to the one before took longer than the interval.
    if (report.mostBehind < 0.5 / options.perSecond)
        text << "Every rewrite went out on schedule (" << report.mostBehind * 1000.0 << " ms behind at most).\n";
    else
        text << "Some rewrites went out late, waiting for answers to the ones before: up to " << report.mostBehind * 1000.0 << " ms behind schedule.\n";

    if (! report.error.empty())
        text << "Stopped early: " << report.error << ".\n";

    text << (report.restored ? "The clip is back as it was (one more history entry).\n" : "The clip couldn't be put back as it was.\n");
    return text.str();
}
}
