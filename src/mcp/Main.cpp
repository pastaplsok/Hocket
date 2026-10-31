// hocket-mcp: connects MCP clients (Claude Code, Claude Desktop) to a running
// Hocket (SPEC §6.3). The client starts it and talks MCP over stdin and
// stdout: newline-delimited JSON-RPC, so stdout carries protocol messages
// only, and everything else goes to stderr.

#include "api/Protocol.h"
#include "api/TokenFile.h"
#include "core/Paths.h"
#include "mcp/Bridge.h"
#include "platform/Platform.h"

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
    void printUsage (std::ostream& out)
    {
        out << "usage: hocket-mcp [--name <name>] [--label <label>] [--token-file <file>]\n"
               "       hocket-mcp --version\n"
               "\n"
               "An MCP server for Hocket. MCP clients such as Claude Code start it themselves and talk\n"
               "to it on stdin and stdout; it forwards their tool calls to the Hocket that's running.\n"
               "See docs/connecting-claude-code.md.\n"
               "\n"
               "  --name <name>        the client's name in Hocket's history. Default: \"Claude\" for\n"
               "                       Claude Code, otherwise the name the client gives\n"
               "  --label <label>      tells two clients with the same name apart, e.g. \"drums\".\n"
               "                       Default: the name of the folder it's started in\n"
               "  --token-file <file>  where to find Hocket's port and token. Default:\n"
               "                       %LOCALAPPDATA%\\Hocket\\api.json\n";
    }

    struct Options
    {
        std::optional<std::string> name;
        std::optional<std::string> label;
        std::optional<std::string> tokenFile;
    };

    std::optional<Options> parseOptions (const std::vector<std::string>& arguments)
    {
        Options options;

        for (std::size_t i = 1; i < arguments.size(); ++i)
        {
            const auto hasValue = i + 1 < arguments.size();

            if (arguments[i] == "--name" && hasValue && ! hocket::mcp::fitName (arguments[i + 1]).empty())
                options.name = arguments[++i];
            else if (arguments[i] == "--label" && hasValue)
                options.label = arguments[++i];
            else if (arguments[i] == "--token-file" && hasValue && ! arguments[i + 1].empty())
                options.tokenFile = arguments[++i];
            else
                return {};
        }

        return options;
    }

    void writeLog (const std::string& line)
    {
        std::cerr << "hocket-mcp: " << line << std::endl;
    }
}

int main (int argc, char* argv[])
{
    hocket::platform::useBinaryStandardStreams();
    const auto arguments = hocket::platform::utf8Arguments (argc, argv);

    if (arguments.size() == 2 && arguments[1] == "--version")
    {
        std::cout << "hocket-mcp " << hocket::api::hocketVersion() << " (API build " << hocket::api::buildFingerprint() << ")\n";
        return 0;
    }

    if (arguments.size() == 2 && (arguments[1] == "--help" || arguments[1] == "-h"))
    {
        printUsage (std::cout);
        return 0;
    }

    const auto options = parseOptions (arguments);

    if (! options)
    {
        printUsage (std::cerr);
        return 1;
    }

    hocket::mcp::BridgeOptions bridgeOptions;
    bridgeOptions.name = options->name.value_or ("");
    bridgeOptions.label = options->label ? *options->label : hocket::mcp::workingFolderName();
    bridgeOptions.session = hocket::mcp::makeSessionId();
    bridgeOptions.log = writeLog;

    if (options->tokenFile)
        bridgeOptions.tokenFile = hocket::pathFromUtf8 (*options->tokenFile);
    else if (const auto file = hocket::api::defaultTokenFilePath())
        bridgeOptions.tokenFile = *file;

    if (bridgeOptions.session.empty())
    {
        writeLog ("couldn't get random bytes from the OS for the session ID");
        return 1;
    }

    // Tools are still listed without a token file; calls then say Hocket isn't running.
    writeLog (std::string (hocket::api::hocketVersion()) + " (API build " + bridgeOptions.fingerprint + "), session " + bridgeOptions.session
         + (bridgeOptions.label.empty() ? "" : ", label '" + bridgeOptions.label + "'") + ", token file "
         + (bridgeOptions.tokenFile.empty() ? "unknown (no %LOCALAPPDATA%)" : hocket::utf8FromPath (bridgeOptions.tokenFile)));

    hocket::mcp::Bridge bridge (std::move (bridgeOptions));
    std::string line;

    while (std::getline (std::cin, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();

        // Some writers start with a UTF-8 byte order mark (.NET's stdin does).
        if (line.starts_with ("\xEF\xBB\xBF"))
            line.erase (0, 3);

        if (line.find_first_not_of (" \t") == std::string::npos)
            continue;

        if (auto reply = bridge.handleMessage (line); ! reply.empty())
        {
            reply += '\n';
            std::fwrite (reply.data(), 1, reply.size(), stdout);
            std::fflush (stdout);
        }
    }

    writeLog ("stdin closed; stopping");
    return 0;
}
