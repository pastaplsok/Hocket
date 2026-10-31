// hocket-stress: rewrites a clip in the running Hocket again and again, to
// check that it keeps up (Milestone 0's stress test, SPEC §10.0). See
// StressClient.h.

#include "api/Protocol.h"
#include "api/TokenFile.h"
#include "core/Paths.h"
#include "platform/Platform.h"
#include "stress/StressClient.h"

#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
    void printUsage (std::ostream& out)
    {
        out << "usage: hocket-stress [--clip <id>] [--count <n>] [--rate <per second>] [--vary] [--token-file <file>]\n"
               "\n"
               "Rewrites a clip in the running Hocket again and again, to check that it keeps up\n"
               "(Milestone 0's stress test). Start hocket-headless --play first, and listen while it runs.\n"
               "By default only a comment changes, so the music shouldn't: any click or gap is a glitch.\n"
               "When it's done, the clip's text is put back as it was.\n"
               "\n"
               "  --clip <id>          the clip to rewrite. Default: clp-bass\n"
               "  --count <n>          how many rewrites. Default: 200\n"
               "  --rate <per second>  how fast. Default: 20\n"
               "  --vary               move every other rewrite's notes up an octave, so notes really change\n"
               "  --token-file <file>  where to find Hocket's port and token. Default:\n"
               "                       %LOCALAPPDATA%\\Hocket\\api.json\n"
               "\n"
               "It exits with 0 if every rewrite was applied and the clip put back, 1 otherwise.\n"
               "For the xrun count, stop hocket-headless with Ctrl+C afterwards: it prints the driver's.\n";
    }

    template <typename Number>
    std::optional<Number> parseNumber (const std::string& text)
    {
        Number value {};
        const auto [end, error] = std::from_chars (text.data(), text.data() + text.size(), value);

        if (error != std::errc() || end != text.data() + text.size())
            return {};

        return value;
    }

    struct Arguments
    {
        hocket::stress::Options options;
        std::string tokenFile;
    };

    std::optional<Arguments> parseArguments (const std::vector<std::string>& arguments)
    {
        Arguments parsed;

        for (std::size_t i = 1; i < arguments.size(); ++i)
        {
            const auto hasValue = i + 1 < arguments.size();

            if (arguments[i] == "--vary")
            {
                parsed.options.vary = true;
            }
            else if (arguments[i] == "--clip" && hasValue && ! arguments[i + 1].empty())
            {
                parsed.options.clipId = arguments[++i];
            }
            else if (arguments[i] == "--count" && hasValue)
            {
                const auto count = parseNumber<int> (arguments[++i]);

                if (! count || *count < 1 || *count > 100000)
                    return {};

                parsed.options.count = *count;
            }
            else if (arguments[i] == "--rate" && hasValue)
            {
                const auto rate = parseNumber<double> (arguments[++i]);

                if (! rate || *rate <= 0.0 || *rate > 1000.0)
                    return {};

                parsed.options.perSecond = *rate;
            }
            else if (arguments[i] == "--token-file" && hasValue && ! arguments[i + 1].empty())
            {
                parsed.tokenFile = arguments[++i];
            }
            else
            {
                return {};
            }
        }

        return parsed;
    }
}

int main (int argc, char* argv[])
{
    const auto arguments = hocket::platform::utf8Arguments (argc, argv);

    if (arguments.size() == 2 && (arguments[1] == "--help" || arguments[1] == "-h"))
    {
        printUsage (std::cout);
        return 0;
    }

    const auto parsed = parseArguments (arguments);

    if (! parsed)
    {
        printUsage (std::cerr);
        return 1;
    }

    const auto& options = parsed->options;
    std::filesystem::path tokenFile;

    if (! parsed->tokenFile.empty())
        tokenFile = hocket::pathFromUtf8 (parsed->tokenFile);
    else if (const auto file = hocket::api::defaultTokenFilePath())
        tokenFile = *file;

    hocket::api::Client client;

    if (const auto error = hocket::stress::connect (client, tokenFile, "stress test", hocket::api::makeSessionId()); ! error.empty())
    {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << "Rewriting " << options.clipId << " " << options.count << " times at " << options.perSecond << " a second"
              << (options.vary ? ", moving the notes up an octave every other time" : "; only a comment changes, so the music shouldn't")
              << "." << std::endl;

    const auto report = hocket::stress::run (client, options);
    std::cout << hocket::stress::summary (report, options);

    return report.error.empty() && report.applied == options.count && report.restored ? 0 : 1;
}
