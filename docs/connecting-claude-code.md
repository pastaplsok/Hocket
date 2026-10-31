# Connecting Claude Code to Hocket

Claude Code works in Hocket through `hocket-mcp`, a small MCP server that Claude Code starts by itself and talks to over stdin and stdout. hocket-mcp lists Hocket's tools, and forwards each tool call to the running Hocket's API on 127.0.0.1, which it finds through Hocket's token file (SPEC §6.3). Hocket never sees your Claude account (SPEC §6.6).

You register it once. From then on, every Claude Code session has Hocket's tools, whether or not Hocket is running; a call made while it isn't says so.

## 1. Install hocket-mcp

Build Hocket (see CLAUDE.md), then, from the repo root in PowerShell:

```
cmake --install build\vs2026 --config Release --component mcp --prefix "$env:LOCALAPPDATA\Hocket"
```

This copies `hocket-mcp.exe` to `%LOCALAPPDATA%\Hocket\bin`. Claude Code runs that copy rather than the one in `build\`: it keeps hocket-mcp running while a session is open, and Windows won't let a build replace a program that's running.

## 2. Register it with Claude Code

```
claude mcp add --scope user hocket -- "$env:LOCALAPPDATA\Hocket\bin\hocket-mcp.exe"
```

`--scope user` makes it available in every folder. `claude mcp list` should then show `hocket: ... ✔ Connected`, even with Hocket not running. In a session, `/mcp` lists hocket and its tools.

## 3. Work on a song

1. Start Hocket on the project: `hocket-headless --play <project folder>` (CLAUDE.md has the device options).
2. Start Claude Code, and ask: "read the bass clip and make bar 2 busier".

Worth knowing:
- **Who did what.** Hocket's history tags each change with its author. Claude Code appears as "Claude", labelled with the name of the folder the session started in: "Claude (my-song)". Each session is its own author, so two sessions never take back each other's changes by accident.
- **Undo.** Claude's `undo` only takes back Claude's own changes. If the newest change is yours, it refuses and says so, unless you ask it to force.
- **Start and stop Hocket whenever you like.** hocket-mcp finds it again on the next call, as the same author. After a restart, the first result tells Claude that the history has started over and the project may have changed. Milestone 0's Hocket never saves, so changes made through the API are gone after a restart.

## Options

Add them after the program in `claude mcp add`; to change them later, `claude mcp remove --scope user hocket` and add it again.

| Option | |
|---|---|
| `--name <name>` | The name in the history, instead of "Claude". Other MCP clients appear under the name they give, unless this is set. |
| `--label <label>` | The label, instead of the folder name. `--label ""` for none. |
| `--token-file <file>` | Where to find Hocket's port and token, instead of `%LOCALAPPDATA%\Hocket\api.json`. Used by the tests. |

## After rebuilding Hocket

hocket-mcp and Hocket must come from the same build once the tools or the API change. When they don't, every tool call fails with a message like "hocket-mcp and this Hocket come from different builds of version 0.0.1: their tools or protocol differ". `hocket-mcp --version` and `hocket-headless --version` both print an API build; they must match.

To update, install again (step 1), then restart hocket-mcp: start a new Claude Code session, or run `/mcp`, pick hocket and reconnect. Installing works even while sessions run the old copy: it's moved aside as `hocket-mcp.old-*.exe`, and a later install deletes it once nothing runs it.

## When something's wrong

- **"Hocket isn't running"**: start `hocket-headless --play`. The token file, `%LOCALAPPDATA%\Hocket\api.json`, exists only while it runs.
- **hocket is missing from `/mcp`, or fails to connect**: check `claude mcp list`, and the path in the registration.
- **Logs**: Hocket prints an `API:` line for each connection and change. hocket-mcp writes a line to stderr for each connection and failure, which Claude Code keeps in its MCP logs. With Claude Code 2.1 they're in `%LOCALAPPDATA%\claude-cli-nodejs\Cache\<folder>\mcp-logs-hocket\`, where `<folder>` is the session's folder with `:` and `\` turned into `-`, e.g. `D--Music-my-song`.

## Other MCP clients

Any MCP client that runs stdio servers can run the same program with the same options, e.g. Claude Desktop from its config file, or a local-model agent (SPEC §6.6).
