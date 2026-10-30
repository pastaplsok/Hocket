# Progress

Status board for the current milestone. Read it at the start of a session; update it at the end of each step. Keep it current, not historical — git log is the history.

## Milestone 0 — headless proof (SPEC §10.0)

| # | Step | Status |
|---|---|---|
| 1 | Build skeleton: pinned submodules, CMake + VS 2026 presets, `hocket-headless --list-devices` | ✅ Done |
| 2 | `.notes` parser + canonical writer (M0 subset), round-trip / error / property / fuzz tests | ✅ Done |
| 3 | `project.json` loader + canonical writer, project model, `hocket-headless --check`, tests | ✅ Done |
| 4 | Command bus: `write_clip`, undo/redo with the AI author check, history, author tags, transactions; tests | ✅ Done |
| 5 | Engine sync + playback: Edit built from the model, 4OSC, looping over ASIO; transport commands; offline-render test | ✅ Done, except the by-ear check over ASIO (below) |
| 6 | WebSocket API on 127.0.0.1 + token file; interrupted-transaction handling | ✅ Done; the by-ear check of `write_clip` over the API moves to step 7 |
| 7 | `hocket-mcp` stdio bridge + doc on connecting Claude Code | ⏭ Next |
| 8 | Stress test, acceptance run, SPEC updates | ⬜ |

**Milestone 0 is done when:**
- a fresh clone builds with the commands in CLAUDE.md;
- the demo project loops over ASIO;
- from Claude Code, `write_clip` is heard on the next loop with no glitch, `undo` restores the old clip, and `history` shows who made each change;
- 200 rewrites at 20/sec during playback cause zero xruns (where the driver reports them) and no audible dropouts;
- all tests pass.

## What exists
- `src/core` (`hocket_core`, no JUCE): `notes/` (`.notes` model, parser, canonical writer), `project/` (`project.json` model, reader, writer, and lookups like `findClip` and `songEnd`; `ClipFileWatcher` for `--watch`), `commands/CommandBus` (the only way to change a `project::Project`; authors are `Author { name, session, label }`, compared by name + session; `interruptTransaction`, `openTransaction`; also routes transport commands to the engine), `Paths` (UTF-8 ⇄ path, `readTextFile`).
- `src/platform` (`hocket_platform`): `Platform.h` with `Platform_windows.cpp` / `Platform_posix.cpp`: secure random bytes, process ID and program name, "is this program still running", `%LOCALAPPDATA%`, and the TCP port probe. The POSIX file has never been compiled.
- `src/engine` (`hocket_engine`): `EditSync` builds a Tracktion `Edit` from the bus's project, rewrites a Tracktion clip on each `ClipContentChanged`, and is the bus's `Transport` (play loops the whole song).
- `src/api` (`hocket_api`, no JUCE): the WebSocket API (SPEC §6.3).
  - `Handler`: JSON-RPC in, reply out; `hello` and the token; per-client state; interrupts on disconnect and in `checkIdle()`.
  - `Tools`: the 9 Milestone 0 tools, their definitions and argument checks.
  - `TextChanges`: write_clip's "stored differently" summary.
  - `Server`: IXWebSocket glue; posts every message to the message thread.
  - `TokenFile`: `api.json` read/write, `makeToken`, `runningInstance`.
- `src/app` (`hocket-headless`): `--list-devices`, `--check <folder>`, `--play <folder> [--driver] [--device] [--watch] [--user]`, `--version`. `--play` serves the API, writes the token file, and calls `checkIdle()` from its 50 ms timer. `Console_windows.cpp` is the console's platform layer (UTF-8 console, wide command line, Ctrl+C).
- Tests (`ctest`, about 11 s):
  - `tests/core` (`hocket_core_tests`): 75 cases.
  - `tests/engine` (`hocket_engine_tests`, own main, one shared Tracktion `Engine`): 14 cases. Edit structure; offline renders (notes start where `.notes` says; `write_clip` and undo move them); live playback through Tracktion's hosted audio device (looping, a mid-loop rewrite heard by the next pass, a held note not retriggered by a rewrite, plus a control showing that check catches a retrigger).
  - `tests/api` (`hocket_api_tests`): 40 cases. The handler driven directly (hello and the token, every tool, argument errors, interrupts on disconnect and idle with a fake clock), `TextChanges`, the token file and platform functions, and end-to-end tests: an IXWebSocket client talking to the server on 127.0.0.1 (ports from 46300), with the test thread as the message thread.
  - `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder.
  - `tests/common/TemporaryFolder.h`: non-ASCII temp folders.
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.

## Still open in step 5: the by-ear check over ASIO
Not done yet: it needs Lee at the laptop with the Magni. `--play` has only been run silently here (an empty clip over WASAPI): the device opens, it loops, `--watch` applies saves (errors print as `file:line:column`), and Ctrl+C stops cleanly. In step 6, the API was also driven silently against a running `--play` (empty clips only): every tool, a wrong token, an interrupted transaction and its note on reconnect, a second instance refused, a clean Ctrl+C removing the token file, and a killed process's stale file replaced on the next start.
1. `hocket-headless.exe --list-devices`, and check the Magni shows up under Windows Audio.
2. `hocket-headless.exe --play examples\m0-demo --driver ASIO --device "ASIO4ALL v2" --watch`, with ASIO4ALL routed to the Magni in its panel.
3. While it loops, edit `examples\m0-demo\clips\bass.notes` and save. Listen for the change landing by the next pass, with no click, gap or stutter. Try a save with a mistake in it; it should print the error and keep playing the old clip.
4. Ctrl+C. Note whether it prints an xrun count (ASIO drivers usually report one; WASAPI doesn't).
5. Revert `bass.notes` afterwards (`git checkout examples/m0-demo/clips/bass.notes`).

## Test hardware
- **Laptop** (main development machine): Magni Mesh DAC, used at the desk. It has no ASIO driver of its own, so ASIO goes through ASIO4ALL. Milestone 0's ASIO checks (steps 5 and 8) run here.
- **Desktop**: Focusrite Scarlett 2i2 with Focusrite's native ASIO driver and mic inputs. It'll be the test interface for v1 recording.

## Next: step 7 — `hocket-mcp`
**Goal** (SPEC §6.3): a small stdio MCP server that Claude Code runs. It lists Hocket's tools itself, and forwards each `tools/call` to the running DAW's API, found through the token file. Step 7 is done when:
- Claude Code, with `hocket-mcp` registered once, lists Hocket's tools whether or not Hocket is running, and calls them against a running `--play`;
- `write_clip` from Claude Code is heard on the next loop (the by-ear check below), `undo` restores the old clip, and `history` shows who made each change;
- a call while Hocket isn't running returns a clear "Hocket isn't running" tool error, and the bridge finds the DAW again when it starts or restarts, keeping its session ID;
- a bridge and a DAW from different builds fail at `hello` with a clear message;
- the tests cover all of this, and a doc says how to connect Claude Code.

### What step 6 gives it
- `api::toolDefinitions` (`src/api/Tools.h`): the tool list, the same code the DAW serves.
- `api::readTokenFile`, `api::runningInstance` (`src/api/TokenFile.h`): the port and token, and whether the DAW that wrote them is still running.
- The protocol (SPEC §6.3): `hello {token, client, session, label?}` first, then `tools/call`, whose results are already MCP-shaped. IXWebSocket's client sends `Origin: ws://…`, which the server accepts.
- `platform::secureRandomBytes` for the session ID: generate it once at startup, as hex, and send the same one on every reconnect, so the client stays the author of its own entries.

### Decided (Lee)
- **Display name:** from MCP `initialize`'s `clientInfo.name`. Claude Code maps to "Claude"; check the exact string it sends (expected "claude-code"). Any other client's name is used as given. `--name <name>` overrides both. It must fit `hello`'s rules: 1 to 64 bytes, no control characters.
- **Label:** `--label <label>`; otherwise the name of the working folder, if available.
  - Claude Code should start stdio servers in the project folder, so that's the bridge's current directory; check this. If it isn't, MCP's `roots/list` is the other source.
  - Read it through the platform layer (wide API, then UTF-8). Skip a drive root or an empty name; shorten it to fit `hello`'s 64 bytes.
- **The bridge always starts and always lists tools.**
  - It compiles in `api::toolDefinitions` from `hocket_api` and answers `tools/list` itself, without the DAW.
  - `begin_transaction`'s description gives the idle timeout, so the bridge and the DAW must use the same value. Make the 10 minutes a shared constant in `hocket_api`; today it's only `HandlerOptions::idleTimeout`'s default.
- **Finding the DAW:** re-read the token file on every connection attempt (`api::runningInstance`), so a restarted DAW, with a new port and token, is found. Connect on the first tool call, and again after a disconnect.
- **When Hocket isn't running,** a call returns an `isError` result such as "Hocket isn't running: start it with `hocket-headless --play <project folder>`, then try again". The same goes for a token file left by a crash, or a refused connection.
- **Version check in `hello`:** the bridge sends its version and a fingerprint; the DAW refuses a mismatch with a clear error and closes the connection, and the bridge returns that error from the tool call.
  - **What's compared:** the Hocket version, plus a hash of a protocol version constant and `toolDefinitions()` as JSON. The version alone stays 0.0.1 through development; the hash catches changed tools and handshake or format changes (bump the protocol constant for those). Each side computes it from its own compiled-in code, so it lives in `hocket_api` next to the tool definitions.
  - **The error says what to do:** e.g. "hocket-mcp and this Hocket come from different builds (tools or protocol differ). Rebuild hocket-mcp from the same source as Hocket and reinstall it, then restart the MCP client." Give both versions when they differ.
  - The DAW's handler refuses unknown `hello` params, so both sides change together.
- **SPEC §6.3** gets updated with the build: `hello`'s new params, the bridge serving the tool list itself (still one list in the source), and the two errors.

### Suggested shape
- `src/mcp` (`hocket-mcp`): links `hocket_api` and `hocket_platform`, IXWebSocket's client and nlohmann/json; no JUCE.
- MCP over stdio is newline-delimited JSON-RPC. stdout carries only protocol messages, so anything else goes to stderr. On Windows, put stdout and stdin in binary mode (`_setmode (_fileno (stdout), _O_BINARY)`), or `\n` becomes `\r\n`.

### The by-ear check (with Lee, after step 7 builds)
`--play examples\m0-demo --driver ASIO --device "ASIO4ALL v2"`, then from Claude Code: rewrite `clp-bass` and listen for it on the next pass with no click or gap; `undo`; `history`. This also completes step 5's check.

## Loose ends
- **Tracktion stops playback when it rebuilds its device lists.** Applying a new MIDI device list (or an audio device change) releases every playback context, which stops the playhead, and the transport then stops itself. `--play` works around the one that always happens: Tracktion's first MIDI scan otherwise lands 4 s after startup (see Gotchas). A MIDI device plugged in during playback would still stop it. Milestone 0 can live with that; v1, with MIDI input and device hot-plug, needs an answer (keep playing, or restart).
- **API (step 6):**
  - `history` has no author filter yet (SPEC §6.4).
  - The person's display name is `--user` only; it becomes a saved setting with the GUI's settings.
  - When the server shuts down, open API transactions aren't interrupted: the process is exiting, and Milestone 0 doesn't save history.
  - A client whose last connection closed with an undelivered note keeps a small entry in the handler until it reconnects. It's never pruned.
  - No limit on message size: IXWebSocket has none. Localhost only, so it's left.
  - Replies are sent from the message thread, and IXWebSocket's server sends block until written. With the 2-second send timeout, a client that stops reading can hold up the message thread for at most that long before it's disconnected. The audio thread is never affected.
- **Command bus:** continuous-gesture coalescing (SPEC §3.1) isn't built; nothing sends continuous parameter changes yet, so add it with the first parameter command. SPEC §6.4 has no MCP `redo`; if one is added, it gets the same author check as `undo`, applied to whoever did the undo.
- **Sync coverage:** `EditSync` handles `ClipContentChanged` only, since `write_clip` is the only project command. Each new command that changes the project needs its sync code and a render or live test. `EditSync` asserts that clips are never added or removed.
- **Tracktion's undo** can't be fully off: JUCE keeps at least one step even at 0 levels, and some Tracktion setters (tempo, clip position) record into it. `EditSync` clears it after every sync; nothing ever undoes in the Edit.
- **Render speed.** An offline render of a few seconds takes about 1.6 s (Release), and each live test about 0.5 s. That's most of the engine tests' 10 s. The cost looks like 4OSC building its wavetables each time it initialises ("Generating waves" in the Debug log). It doesn't matter for Milestone 0, but it will for `render_section` and the analysis tools (SPEC §7).
- **Debug engine tests:** no JUCE assertions fire and JUCE's leak detector is silent, but the MSVC CRT reports two to four 16-byte blocks at exit, even when no test case runs. Not chased.
- **`--watch`** compares each file's modification time and size. A same-size rewrite within the file system's timestamp resolution could be missed, which is unlikely in practice.
- **Saving to disk:** `project::write()` produces every file in memory; writing them to the folder (atomically, plus SPEC §6.1's external-change detection) isn't built. Milestone 0 doesn't need it.
- **Tracktion pin:** `develop` at 3.5.0 is untagged. Move to the 3.5.0 tag when it exists (JUCE pin follows; see DEPENDENCIES.md).
- **Upstream, worth reporting:**
  - Tracktion's `Engine::getVersion()` is hardcoded to "v3.1.0"; we read `VERSION.md` instead.
  - IXWebSocket's server sets `SO_REUSEADDR` unconditionally (`IXSocketServer.cpp:135`), so on Windows a second server can bind a port another is listening on. `listen()` is non-virtual and the socket private, so `SO_EXCLUSIVEADDRUSE` can't be set without forking. Also, its server sends block with no timeout by default (`kSendTimeoutSeconds = -1`).
  - `DeviceManager::initialise()` calls `rescanMidiDeviceList()` (a 5 ms timer) and then `restartMidiCheckTimer()`, which replaces it with the 4 s rescan timer. So the first MIDI scan lands 4 s after startup and stops anything already playing. A forum post is drafted in Lee's `Documents\Hocket\upstream\tracktion-first-midi-scan-stops-playback.md`, not yet posted; once it is, put the link here.

## Gotchas picked up so far
- **Line endings.** This machine has `core.autocrlf=true`. `.gitattributes` keeps `*.notes`, `*.auto` and `project.json` LF; add any new byte-compared fixture type there. In the Bash tool, `grep $'\r'` reports every line; count CRs with `tr -cd '\r' < file | wc -c`.
- **Don't name a helper `quoted`.** Argument-dependent lookup finds `std::quoted` instead (error C2676). The code uses `singleQuoted` / `doubleQuoted`.
- **nlohmann/json** is built with implicit conversions off: write `get<T>()`.
- **Property tests against the bus** must canonicalize their reference copy with `*notes::parse (text).clip`, because the bus stores the parsed clip (sorted, spellings filled in), not the text.
- **Build time.** JUCE and Tracktion compile once into `hocket_juce`; a clean Release build takes about a minute here, a fresh clone plus build about four.
- **Tracktion's first MIDI scan.** Before playing, call `engine.getDeviceManager().rescanMidiDeviceList()` and run the message loop briefly (`runDispatchLoopUntil (100)`), as `--play` does. Otherwise the scan lands 4 s after startup and stops playback. The engine tests never see this: the hosted device has `useMidiDevices = false`.
- **`JUCE_MODAL_LOOPS_PERMITTED=1`** (set on `hocket_juce`) makes `MessageManager::runDispatchLoopUntil` available. `runDispatchLoop` can only run once per process: after `stopDispatchLoop` it returns at once.
- **One Tracktion `Engine` at a time.** The engine tests share `testing::testEngine()`, which never opens a real device and keeps its settings in a temp folder. A `FakeAudioDevice` must be opened before the `EditSync` whose Edit plays on it.
- **Live tests and graph rebuilds.** Tracktion applies a change to the playback graph on a 1 ms message-thread timer. In single-threaded tests, call `edit.dispatchPendingUpdatesSynchronously()` to apply it at an exact point.
- **4OSC isn't deterministic** from run to run (oscillator phase), so compare loudness between runs, not samples.
- **Real-time harness, for step 8's stress test.** A `std::thread` calling `FakeAudioDevice::play (blockSize)` at real-time pace, while the test thread runs `runDispatchLoopUntil` and sends commands, behaves like the app. It was tried while debugging step 5 and worked.
- **Tracktion renders** (`Renderer::renderToFile`) stop all transports, and they need a non-empty track bitset (`toBitSet (getAllTracks (edit))`), whatever the docs say about empty meaning all.
- **Shell edits of C++ string literals.** In the Bash tool, `\\n` inside a heredoc can arrive as a real newline. Make code edits with the Edit tool.
- **Windows port probes** (measured on this machine; used by `platform::isTcpPortFree`):
  - A socket with `SO_EXCLUSIVEADDRUSE` bound to 0.0.0.0 fails against any listener on the port: loopback with `SO_REUSEADDR` (like IXWebSocket), loopback plain, or wildcard. It succeeds when the port has only `TIME_WAIT` connections.
  - A plain bind to 127.0.0.1 misses wildcard listeners, and a plain bind to 0.0.0.0 misses loopback ones.
- **IXWebSocket's client always sends an `Origin` header** (`ws://host:port`) unless you set your own; it can't be left out. The server's web-page check accepts `ws://` and `wss://` origins for that reason.
- **IXWebSocket servers send in blocking mode** with no timeout by default; pass `sendTimeoutSeconds` (`Server.cpp` uses 2).
- **nlohmann `json` sorts object keys**; use `ordered_json` where order matters (the project writer does). The token file's keys come out alphabetical, which is fine.
- **Stopping a background `--play` cleanly from the tools.** A process started from the Bash or PowerShell tool can't be sent Ctrl+C, and killing it skips the clean exit.
  - Start it through `cmd /c` with `ProcessStartInfo.CreateNoWindow = true`: a console of its own, no window, output redirected to a file.
  - To stop it, run a separate `powershell` that calls `FreeConsole`, `AttachConsole (cmd's PID)`, `SetConsoleCtrlHandler (null, true)` and `GenerateConsoleCtrlEvent (CTRL_C_EVENT, 0)` through `Add-Type`.
  - For a silent check, use a copy of the demo with an empty clip, and only ever write empty clips.
