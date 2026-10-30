#pragma once

// The API's tools (SPEC §6.4): their MCP definitions, and running them for one
// client against the command bus. Tool definitions live here, next to the bus,
// so hocket-mcp only forwards them (SPEC §6.3). Descriptions, results and
// errors are written for an LLM reader.

#include "core/commands/CommandBus.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hocket::api
{
    struct ToolResult
    {
        std::vector<std::string> content; // one text block each
        bool isError = false;
    };

    // MCP tool definitions: name, description, inputSchema and annotations.
    // idleTimeout is how long a transaction stays open without a request from
    // its owner; begin_transaction's description says so.
    nlohmann::json toolDefinitions (std::chrono::seconds idleTimeout);

    // Runs a tool for the caller. nullopt if there's no tool with that name.
    // Bad arguments give an error result, so the model can correct them.
    std::optional<ToolResult> callTool (commands::CommandBus& bus, const commands::Author& caller,
                                        std::string_view name, const nlohmann::json& arguments);

    // Whether the tool only reads the project.
    bool isReadOnlyTool (std::string_view name);

    // "10 minutes", "1 minute", "90 seconds"
    std::string durationText (std::chrono::seconds duration);
}
