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
| 7 | `hocket-mcp` stdio bridge + doc on connecting Claude Code | ✅ Built, tested, and run silently against `--play`; registering it in Claude Code and the by-ear check wait for Lee (below) |
| 8 | Stress test, acceptance run, SPEC updates | 🟡 Tooling built and run silently: the engine stress test, `hocket-stress` and `tools/acceptance/Run-Acceptance.ps1`. The stress test found a dropped block about once in 1,000 rewrites (Loose ends). The ASIO runs and by-ear checks are Lee's, on Sunday 2026-11-01 (below); SPEC updates after that |

**Milestone 0 is done when:**
- a fresh clone builds with the commands in CLAUDE.md;
- the demo project loops over ASIO;
- from Claude Code, `write_clip` is heard on the next loop with no glitch, `undo` restores the old clip, and `history` shows who made each change;
- 200 rewrites at 20/sec during playback cause zero xruns (where the driver reports them) and no audible dropouts;
- all tests pass.

## What exists
- `src/core` (`hocket_core`, no JUCE): `notes/` (`.notes` model, parser, canonical writer), `project/` (`project.json` model, reader, writer, and lookups like `findClip` and `songEnd`; `ClipFileWatcher` for `--watch`), `commands/CommandBus` (the only way to change a `project::Project`; authors are `Author { name, session, label }`, compared by name + session; `interruptTransaction`, `openTransaction`; also routes transport commands to the engine), `Paths` (UTF-8 ⇄ path, `readTextFile`).
- `src/platform` (`hocket_platform`): `Platform.h` with `Platform_windows.cpp` / `Platform_posix.cpp`: the command line as UTF-8 (`utf8Arguments`, used by both programs), binary stdin/stdout, secure random bytes, process ID and program name, "is this program still running", `%LOCALAPPDATA%`, and the TCP port probe. The POSIX file has never been compiled.
- `src/engine` (`hocket_engine`): `EditSync` builds a Tracktion `Edit` from the bus's project, rewrites a Tracktion clip on each `ClipContentChanged`, and is the bus's `Transport` (play loops the whole song).
- `src/api` (`hocket_api`, no JUCE): the WebSocket API (SPEC §6.3), its server and its client.
  - `Handler`: JSON-RPC in, reply out; `hello` with the token and the build check (error -32002 for another build); per-client state; interrupts on disconnect and in `checkIdle()`.
  - `Protocol`: what both ends share: `protocolVersion` (bump it for changes to `hello` or the formats), `defaultIdleTimeout` (10 minutes), `hocketVersion()`, `buildFingerprint()` (FNV-1a of the protocol version and `toolDefinitions()`), and `makeSessionId()` for clients.
  - `Client`: a blocking IXWebSocket client, one request at a time; a late response to a timed-out request is dropped. Used by hocket-mcp and hocket-stress.
  - `Tools`: the 9 Milestone 0 tools, their definitions and argument checks.
  - `TextChanges`: write_clip's "stored differently" summary.
  - `Server`: IXWebSocket glue; posts every message to the message thread.
  - `TokenFile`: `api.json` read/write, `makeToken`, `runningInstance`.
- `src/app` (`hocket-headless`): `--list-devices`, `--check <folder>`, `--play <folder> [--driver] [--device] [--watch] [--user]`, `--version` (with the API build). `--play` serves the API, writes the token file, and calls `checkIdle()` from its 50 ms timer. `Console_windows.cpp` is the console's platform layer (UTF-8 console, Ctrl+C).
- `src/mcp` (`hocket_bridge` library, `hocket-mcp` program, no JUCE): the MCP bridge (SPEC §6.3).
  - `Bridge`: MCP JSON-RPC in, reply out. Answers `initialize`, `ping` and `tools/list` itself; forwards `tools/call`, re-reading the token file on every call. Takes the name from `clientInfo` (`claude-code` → "Claude") or `--name`, and the label from `--label` or the working folder. Adds a note to the first result after a restart, and says "Hocket isn't running" when it can't connect.
  - `Main.cpp`: the stdio loop (binary mode; skips blank lines, a trailing CR and a byte order mark), `--name`, `--label`, `--token-file`, `--version`. Logs go to stderr.
  - The `mcp` install component copies it to `<prefix>\bin`. A copy that's running is renamed aside first, and cleaned up by a later install.
- `docs/connecting-claude-code.md`: install, register, use, update, troubleshoot.
- `tools/stress` (`hocket_stress_client` library, `hocket-stress` program, no JUCE): Milestone 0's stress test over the API.
  - It rewrites one clip (`--clip`, default `clp-bass`) `--count` times (200) at `--rate` per second (20), then puts the clip's text back.
  - By default only a comment line changes, so the music shouldn't, and any click or gap is a glitch. `--vary` moves every other rewrite's notes up an octave.
  - It reports answer times (median, 95th percentile, slowest) and how far sends fell behind schedule. It exits 1 if any rewrite wasn't applied or the clip wasn't put back.
- `tools/acceptance/Run-Acceptance.ps1`: Milestone 0's acceptance checks that don't need ears, run silently on a copy of the demo with an empty clip. It prints PASS, FAIL or NOTE for each check, keeps its logs in a temp folder, and exits 1 on any failure.
  - The checks: the tests (with the engine stress test's numbers); `--play` loops; the MCP path as Claude Code runs it; `hocket-stress`; a clean Ctrl+C with the xrun count.
  - `-Driver`/`-Device` go to `--play`. `-FreshClone` (with `-CloneFrom`) also clones and builds with CLAUDE.md's commands. `Send-CtrlC.ps1` stops `--play` the way Ctrl+C does.
- Tests (`ctest`, about 45 s, most of it the stress tests running in real time):
  - `tests/core` (`hocket_core_tests`): 75 cases.
  - `tests/engine` (`hocket_engine_tests`, own main, one shared Tracktion `Engine`): 16 cases.
    - Edit structure; offline renders (notes start where `.notes` says; `write_clip` and undo move them).
    - Live playback through Tracktion's hosted audio device: looping, a mid-loop rewrite heard by the next pass, a held note not retriggered by a rewrite, plus a control showing that check catches a retrigger.
    - Two stress tests (`StressTests.cpp`, about 18 s) in a real-time harness. An `AudioThread` plays `FakeAudioDevice::playBlock` at a sound card's pace with real-time priority, timing each block, while the test thread is the message thread.
      - 200 comment-only rewrites at 20/s to a held note. Checked: no block over its 5.8 ms budget (Release only), and no gap, retrigger or click against the first, untouched pass. Dropped blocks are counted and only warned about (see Loose ends). A control damages a copy to show each check can fail.
      - 21 rewrites that move a note, with no block over budget, and the last rewrite is what plays.
  - `tests/api` (`hocket_api_tests`): 44 cases. The handler driven directly (hello, the token and the build check, every tool, argument errors, interrupts on disconnect and idle with a fake clock), the fingerprint, `TextChanges`, the token file and platform functions, and end-to-end tests: an IXWebSocket client talking to the server on 127.0.0.1 (ports from 46300), with the test thread as the message thread.
  - `tests/mcp` (`hocket_mcp_tests`): 10 cases. The bridge against a real `Server` and `Handler` on a thread of their own (`FakeHocket`, ports from 46320, token file in a non-ASCII temp folder). Covers: tools listed without Hocket; not running (no file, a stale file, nothing listening); write, undo and history as the client's own author; names and labels; a restart (same session, note once); another build; a timeout with a late response dropped; protocol errors.
  - `hocket_mcp_stdio` (CTest, `tests/mcp/StdioTest.cmake`): runs `hocket-mcp.exe` on piped stdin with a byte order mark, a blank line and a CRLF line. Checks one LF-terminated JSON reply per request and nothing else on stdout, and that the non-ASCII `--token-file` and `--label` arrive intact.
  - `tests/stress` (`hocket_stress_tests`): 5 cases. The rewrites' text (steady and varied); a run against `FakeHocket` (ports from 46340): paced, all applied, the clip put back, the history; errors before starting; the summary. `hocket_stress_not_running` (CTest) runs the program with a non-ASCII `--token-file` that doesn't exist.
  - `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder.
  - `tests/common`: `TemporaryFolder.h` (non-ASCII temp folders) and `FakeHocket.h` (a bus, handler and API server on a thread of their own, plus a token file: a running Hocket, as far as a client can tell).
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.

## Waiting for Lee: the ASIO runs and by-ear checks (steps 5, 7 and 8), Sunday 2026-11-01
They need Lee at the laptop with the Magni; Lee plans to do them on Sunday 2026-11-01.

**What's been checked so far, all silent** (an empty clip over WASAPI, only empty clips written):
- `--play`: the device opens, it loops, `--watch` applies saves (errors print as `file:line:column`), and Ctrl+C stops cleanly.
- The API directly (step 6): every tool, a wrong token, an interrupted transaction and its note on reconnect, a second instance refused, a clean Ctrl+C removing the token file, and a killed process's stale file replaced on the next start.
- `hocket-mcp.exe` over stdio (step 7), started in the project folder:
  - "not running" before `--play` starts;
  - write, history, undo and read as `Claude (silent-demo)`;
  - "not running" again after a Ctrl+C;
  - after a second `--play`, it reconnects with the same session ID and adds the restart note once;
  - it exits 0 when stdin closes.
- Claude Code 2.1.314, against a throwaway `CLAUDE_CONFIG_DIR`:
  - `claude mcp add --scope user hocket -- <exe>` and `claude mcp list` show it connected without Hocket;
  - it sends `clientInfo.name` `claude-code` and asks for MCP `2025-11-25`;
  - it starts the server in the session's folder, so the label was `m0-demo`.
- Step 8, on 2026-10-30: `tools/acceptance/Run-Acceptance.ps1` over WASAPI (the laptop's speakers), and every check passed:
  - the tests;
  - `--play` looping;
  - write_clip, undo restoring the clip, history naming `you` and `Lee`, and Claude's undo refusing Lee's change;
  - `hocket-stress`: 200 of 200 applied, answers 0.4 ms median and 3.4 ms at most, the clip put back;
  - `hocket-mcp` exiting on stdin's close, and a clean Ctrl+C.

**To do with Lee:**
1. ✅ Installed and registered (2026-10-30, after the step 7 commit). `hocket-mcp.exe` is in `%LOCALAPPDATA%\Hocket\bin` (API build `7a9b08d34b2a2d3e`, the same as `hocket-headless`). It's registered as `hocket` at user scope, and `claude mcp get hocket` shows it connected. Every session gets the tools, including the ones developing Hocket here; they say "Hocket isn't running" until `--play` runs. After a rebuild that changes the tools or the API, run CLAUDE.md's install command again.
2. Start a new Claude Code session; `/mcp` should list hocket with 9 tools.
3. `hocket-headless.exe --list-devices`, and check the Magni shows up under Windows Audio.
4. `hocket-headless.exe --play examples\m0-demo --driver ASIO --device "ASIO4ALL v2" --watch --user Lee`, with ASIO4ALL routed to the Magni in its panel.
5. From Claude Code: rewrite `clp-bass`, and listen for it on the next pass, with no click, gap or stutter. Then `undo` (the old line comes back), and `history`, which should name Claude with the folder label. `--play` should print `API: Claude (<folder>) connected`.
6. Edit `examples\m0-demo\clips\bass.notes` and save. Listen for the change by the next pass. Save once with a mistake in it: it should print the error and keep playing the old clip. Then ask Claude to `undo`: it should refuse, since the newest change is Lee's.
7. The stress run by ear (step 8). Still playing, in a second terminal: `build\vs2026\tools\stress\Release\hocket-stress.exe`.
   - For the 10 s it runs, the bass line should sound exactly as before, since only a comment changes. Listen for clicks or gaps.
   - Run it three times, and note how many clicks you hear in each run. The engine stress test drops a block about once in 500 to 1,000 rewrites (Loose ends), so a click in one run of 200 would match that known issue.
   - Then once with `--vary`: the notes jump an octave every other rewrite, so it sounds busy, but it should never stutter or stop.
   - Each run should end with "All 200 were applied" and "The clip is back as it was".
8. Ctrl+C. Note the xrun count it prints (ASIO drivers usually report one; WASAPI doesn't). It should be 0.
9. Revert `bass.notes` afterwards (`git checkout examples/m0-demo/clips/bass.notes`).
10. The acceptance checks over ASIO, silently, for an xrun count without listening: `powershell -ExecutionPolicy Bypass -File tools\acceptance\Run-Acceptance.ps1 -Driver ASIO -Device "ASIO4ALL v2"`. Every check should pass, "No xruns" among them, and it prints the engine stress test's dropped-block count.

**To do when Lee sets up Claude Desktop** (Lee is away from home as of 2026-10-30, for a couple of days; don't drop this):
1. Register the installed `hocket-mcp.exe` in Claude Desktop's config. It should be `%APPDATA%\Claude\claude_desktop_config.json`, under `"mcpServers": { "hocket": { "command": "<path to hocket-mcp.exe>" } }`; check against Claude Desktop's own docs. Then restart Claude Desktop.
2. Find the `clientInfo` name it sends. hocket-mcp logs `client '<name>' <version>, MCP <version>` to stderr on `initialize`, and Claude Desktop keeps servers' stderr in its MCP logs (expected in `%APPDATA%\Claude\logs\`, e.g. `mcp-server-hocket.log`). `--play`'s `API: ... connected` line shows the name Hocket ends up using.
3. Map that name to "Claude" in `mcp::authorName` (`src/mcp/Bridge.cpp`). Add a check to "names and labels are made to fit hello" in `tests/mcp/BridgeTests.cpp`. Update SPEC §6.3's `client` line and the "Other MCP clients" section of `docs/connecting-claude-code.md`. Rebuild and reinstall.
4. Check the label too. Claude Desktop isn't started in a project folder, so the working folder's name may mean nothing (an app or system folder). If so, pass `--label` in its config, or pick a better default for clients that don't start the bridge in a project folder.

## Test hardware
- **Laptop** (main development machine): Magni Mesh DAC, used at the desk. It has no ASIO driver of its own, so ASIO goes through ASIO4ALL. Milestone 0's ASIO checks (steps 5 and 8) run here.
- **Desktop**: Focusrite Scarlett 2i2 with Focusrite's native ASIO driver and mic inputs. It'll be the test interface for v1 recording.

## Next: finishing step 8 and Milestone 0
After the checks above. Goal: Milestone 0's "done when" list at the top.
- **The dropped block** (Loose ends) is the one thing in the way of "no audible dropouts". Decide with Lee: dig into Tracktion, work around it, or report it upstream. Once it's fixed, turn the stress test's dropped-block `WARN` back into a `CHECK`.
- **SPEC:** fold in what Milestone 0 taught, and mark §10.0 done.

## Loose ends
- **Dropped blocks under rapid rewrites (found by step 8's stress test).**
  - **What happens:** about once in 500 to 1,000 rewrites, Tracktion plays one block of digital silence: 256 samples, 5.8 ms at 44.1 kHz. It starts on a block boundary, one to three blocks after a rewrite's graph swap. The held note carries on afterwards at full level, so the voice isn't reset; that block's output is simply empty. It would be heard as a click.
  - **How often:** at 200 rewrites at 20/s, about one run in three to five has one. A temporary "hammer" (1,000 rewrites at 100/s) dropped 1 or 2 blocks per run.
  - **Ruled out:**
    - slow blocks: the slowest was 2.7 ms, against a 5.8 ms budget;
    - the plugin being re-initialised: the voice carries on;
    - the node player's try-lock: `LockFreeMultiThreadedNodePlayer::process` only gives up while `processMutex` is held, which only happens when the thread count changes;
    - the fake device: `HostedAudioDeviceInterface::processBlock` hands the block straight to Tracktion's device manager, the same path a sound card's callback takes. So it's likely to happen on real hardware too.
  - **Not settled:** `EditSync::clipContentChanged` calls `setLoopRange` on every rewrite, even when the range hasn't changed. Without that call, the hammer dropped 1 block in 4 runs, against 5 with it. That's suggestive, not conclusive; setting an unchanged value shouldn't notify anything.
  - **One unexplained failure:** on 2026-10-30, a Debug run of the two stress tests failed once, right after a full Debug `ctest`. My filter hid the error, so which check failed isn't known. It didn't happen again in 23 Debug runs since: 11 of the stress tests alone, and 12 of the whole engine suite. If it shows up again, keep the whole output.
  - **Next:** instrument Tracktion locally to find which node outputs the empty block, and whether it's the first block after a graph swap. Check whether upstream knows about it. Then fix it, work around it (only set the loop range when it changes, or coalesce rewrites), or report it. Until then, the stress test only warns about dropped blocks; any other gap, retrigger or click fails it.
- **Tracktion stops playback when it rebuilds its device lists.** Applying a new MIDI device list (or an audio device change) releases every playback context, which stops the playhead, and the transport then stops itself. `--play` works around the one that always happens: Tracktion's first MIDI scan otherwise lands 4 s after startup (see Gotchas). A MIDI device plugged in during playback would still stop it. Milestone 0 can live with that; v1, with MIDI input and device hot-plug, needs an answer (keep playing, or restart).
- **API (step 6):**
  - `history` has no author filter yet (SPEC §6.4).
  - The person's display name is `--user` only; it becomes a saved setting with the GUI's settings.
  - When the server shuts down, open API transactions aren't interrupted: the process is exiting, and Milestone 0 doesn't save history.
  - A client whose last connection closed with an undelivered note keeps a small entry in the handler until it reconnects. It's never pruned.
  - No limit on message size: IXWebSocket has none. Localhost only, so it's left.
  - Replies are sent from the message thread, and IXWebSocket's server sends block until written. With the 2-second send timeout, a client that stops reading can hold up the message thread for at most that long before it's disconnected. The audio thread is never affected.
- **MCP bridge (step 7):**
  - Only Claude Code's `claude-code` maps to "Claude". Other clients appear under the `clientInfo` name they send, unless their config passes `--name`. Claude Desktop's name gets mapped once it's checked (see "To do when Lee sets up Claude Desktop" above); add others as clients are tried.
  - It handles one request at a time: a call waits for Hocket's answer before the next message is read. That's fine while the DAW handles requests one at a time anyway. `notifications/cancelled` is ignored, and a `ping` sent during a call is answered after it.
  - Replies time out after 30 s, and `hello` uses the same limit. `render_section` and the analysis tools (SPEC §7) may need longer, or progress notifications.
  - Supported MCP versions are 2024-11-05 to 2025-11-25; a client asking for a newer one is offered 2025-11-25. Claude Code 2.1.314 asks for 2025-11-25, but its MCP log calls that `"protocolEra": "legacy"`, so a newer era exists. Check what it changes before a future Claude Code stops accepting the old one.
  - A refused connection to a port on 127.0.0.1 takes about 2 s on Windows. It only happens when the token file names a running Hocket that isn't listening.
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
- **The real-time harness** (`tests/engine/StressTests.cpp`). An `AudioThread` (a JUCE real-time thread) calls `FakeAudioDevice::playBlock` at a sound card's pace, while the test thread runs `runDispatchLoopUntil (1)` and sends commands; it behaves like the app.
  - `playBlock` renders into a buffer allocated up front: JUCE's `AudioBuffer::setSize` reallocates whenever it grows, even with `avoidReallocating` (that only helps when shrinking), so growing `output()` on every block would swamp the timing.
- **Engine tests that run the message loop** receive whatever Tracktion has posted before, and handling a device update stops the transport. A test that ran after another test's device was closed stopped playing within two blocks. `FakeAudioDevice` now handles pending messages when it opens and closes.
- **Tracktion renders** (`Renderer::renderToFile`) stop all transports, and they need a non-empty track bitset (`toBitSet (getAllTracks (edit))`), whatever the docs say about empty meaning all.
- **Shell edits of C++ string literals.** In the Bash tool, `\\n` inside a heredoc can arrive as a real newline, and other backslashes in Python string literals get mangled too (Windows paths). Make code and doc edits with the Edit tool.
- **Windows port probes** (measured on this machine; used by `platform::isTcpPortFree`):
  - A socket with `SO_EXCLUSIVEADDRUSE` bound to 0.0.0.0 fails against any listener on the port: loopback with `SO_REUSEADDR` (like IXWebSocket), loopback plain, or wildcard. It succeeds when the port has only `TIME_WAIT` connections.
  - A plain bind to 127.0.0.1 misses wildcard listeners, and a plain bind to 0.0.0.0 misses loopback ones.
- **IXWebSocket's client always sends an `Origin` header** (`ws://host:port`) unless you set your own; it can't be left out. The server's web-page check accepts `ws://` and `wss://` origins for that reason.
- **IXWebSocket servers send in blocking mode** with no timeout by default; pass `sendTimeoutSeconds` (`Server.cpp` uses 2).
- **nlohmann `json` sorts object keys**; use `ordered_json` where order matters (the project writer does). The token file's keys come out alphabetical, which is fine.
- **Stopping a background `--play` cleanly from the tools.** A process started from the Bash or PowerShell tool can't be sent Ctrl+C, and killing it skips the clean exit.
  - Start it through `cmd /c` with `ProcessStartInfo.CreateNoWindow = true`: a console of its own, no window, output redirected to a file.
  - To stop it, run `tools/acceptance/Send-CtrlC.ps1 <cmd's PID>` in a separate `powershell -File`. It calls `FreeConsole`, `AttachConsole`, `SetConsoleCtrlHandler (null, true)` and `GenerateConsoleCtrlEvent (CTRL_C_EVENT, 0)`. An inline `-Command` script passed through `Start-Process` didn't work.
  - For a silent check, use a copy of the demo with an empty clip, and only ever write empty clips. `Run-Acceptance.ps1` does all of this.
- **Windows PowerShell 5.1 and native programs:** with `$ErrorActionPreference = 'Stop'`, redirecting a native program's stderr (`*>`, `2>&1`) turns its first stderr line into a terminating error (`git clone`'s progress is enough), and `*>` writes UTF-16. `Run-Acceptance.ps1` runs logged programs through `cmd /c "... > log 2>&1"` instead.
- **Windows sleeps are coarse:** `sleep_until` can wake up to about 16 ms late (the default 15.6 ms timer), so `hocket-stress`'s sends can start that far behind schedule. That's well inside its 50 ms interval.
- **Driving `hocket-mcp` from PowerShell:** start it with `ProcessStartInfo`, stdin and stdout redirected, and write one JSON line per request. .NET's redirected stdin starts with a UTF-8 byte order mark, which the bridge skips.
- **Trying an MCP registration without touching Lee's Claude Code config:** set `CLAUDE_CONFIG_DIR` to a scratch folder for the command, then run `claude mcp add --scope user ...` and `claude mcp list` (it starts the server and sends `initialize`). Afterwards, delete the scratch folder and the log folder it created (below).
- **Claude Code's MCP logs**, including each server's stderr: `%LOCALAPPDATA%\claude-cli-nodejs\Cache\<session folder, ':' and '\' as '-'>\mcp-logs-<server>\*.jsonl` (Claude Code 2.1). Wrapping the server in `cmd /c ... 2>>file` doesn't work: Node's argument quoting breaks cmd's redirection.
- **Windows won't overwrite a running `.exe`, but will rename one.** The `mcp` install component relies on this to replace a `hocket-mcp.exe` that Claude Code sessions are running.
