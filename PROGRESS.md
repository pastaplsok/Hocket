# Progress

Status board for the current milestone. Read it at the start of a session; update it at the end of each step. Keep it current, not historical — git log is the history.

## Milestone 0 — headless proof (SPEC §10.0): ✅ done, 2026-11-06

| # | Step | Status |
|---|---|---|
| 1 | Build skeleton: pinned submodules, CMake + VS 2026 presets, `hocket-headless --list-devices` | ✅ Done |
| 2 | `.notes` parser + canonical writer (M0 subset), round-trip / error / property / fuzz tests | ✅ Done |
| 3 | `project.json` loader + canonical writer, project model, `hocket-headless --check`, tests | ✅ Done |
| 4 | Command bus: `write_clip`, undo/redo with the AI author check, history, author tags, transactions; tests | ✅ Done |
| 5 | Engine sync + playback: Edit built from the model, 4OSC, looping over ASIO; transport commands; offline-render test | ✅ Done. Heard looping over FL Studio ASIO on 2026-11-04 (below) |
| 6 | WebSocket API on 127.0.0.1 + token file; interrupted-transaction handling | ✅ Done; the by-ear check of `write_clip` over the API moves to step 7 |
| 7 | `hocket-mcp` stdio bridge + doc on connecting Claude Code | ✅ Done. Registered in Claude Code; `write_clip` and `undo` from Claude Code checked by ear on 2026-11-04 (below) |
| 8 | Stress test, acceptance run, SPEC updates | ✅ Done, 2026-11-06. The stress tests found two Tracktion bugs: stuck notes (worked around) and a dropped block (fixed in Hocket's fork of Tracktion). SPEC v0.6 has what Milestone 0 taught. The code review of 2026-10-31 is fixed, except two items deferred (Loose ends) |

**Milestone 0 was done when** (all met; how, below):
- ✅ a fresh clone builds with the commands in CLAUDE.md;
- ✅ the demo project loops over ASIO;
- ✅ from Claude Code, `write_clip` is heard on the next loop with no glitch, `undo` restores the old clip, and `history` shows who made each change;
- ✅ 200 rewrites at 20/sec during playback cause zero xruns (where the driver reports them) and no audible dropouts;
- ✅ all tests pass.

## What exists
- `src/core` (`hocket_core`, no JUCE):
  - `notes/`: the `.notes` model, parser and canonical writer. A clip is at most 999 bars (`maxLengthInBars`).
  - `project/`: the `project.json` model, reader, writer, and lookups like `findClip` and `songEnd`; `ClipFileWatcher` for `--watch`, which keeps trying a settled file it can't read yet, for up to 2 s.
  - `commands/CommandBus`: the only way to change a `project::Project`. Authors are `Author { name, session, label }`, compared by name + session. A write is a no-op when the clip's canonical text wouldn't change. `interruptTransaction`, `openTransaction`, `changesTo` (the entries that changed a clip); it also routes transport commands to the engine, and `isPlaying()` asks it.
  - `commands/WatchedSaves`: `--watch`'s saves as the person's `write_clip`, with a warning when one replaces a change made some other way since the file and clip last matched.
  - `Paths`: UTF-8 ⇄ path (`utf8FromPath` never throws: an unpaired UTF-16 surrogate becomes U+FFFD), `readTextFile`.
- `src/platform` (`hocket_platform`): `Platform.h` with `Platform_windows.cpp` / `Platform_posix.cpp`: the command line as UTF-8 (`utf8Arguments`, used by both programs), binary stdin/stdout, secure random bytes, process ID and program name, "is this program still running", `%LOCALAPPDATA%`, `FileLock` (one holder at a time; on Windows nothing else can even read the file), and the TCP port probe. The POSIX file has never been compiled.
- `src/engine` (`hocket_engine`):
  - `EditSync` builds a Tracktion `Edit` from the bus's project, rewrites a Tracktion clip on each `ClipContentChanged`, and is the bus's `Transport` (play loops the whole song). Its clips don't use Tracktion's proxy sequences, so they play through `LoopingMidiNode`, which ends the notes a rewrite takes away (Gotchas).
  - `PlaybackDevice`: `openDevice` (from `--driver` and `--device`), and what `--play` says about the device at the start and at Ctrl+C. It asks JUCE's device manager each time, since the manager deletes a device that disappears and opens another. The xrun report has the driver's count (where it keeps one) and JUCE's count of audio callbacks that took longer than their block.
- `src/api` (`hocket_api`, no JUCE): the WebSocket API (SPEC §6.3), its server and its client.
  - `Handler`: JSON-RPC in, reply out; `hello` with the token and the build check (error -32002 for another build); per-client state; interrupts on disconnect and in `checkIdle()`. It never throws: anything thrown while handling a request is answered with -32603.
  - `Protocol`: what both ends share: `protocolVersion` (bump it for changes to `hello` or the formats), `defaultIdleTimeout` (10 minutes), `hocketVersion()`, `buildFingerprint()` (FNV-1a of the protocol version and `toolDefinitions()`), and `makeSessionId()` for clients.
  - `Client`: a blocking IXWebSocket client, one request at a time; a late response to a timed-out request is dropped. Used by hocket-mcp and hocket-stress.
  - `Tools`: the 9 Milestone 0 tools, their definitions and argument checks.
  - `TextChanges`: write_clip's "stored differently" summary.
  - `Server`: IXWebSocket glue; posts every message to the message thread.
  - `TokenFile`: `api.json` read/write, `makeToken`, `runningInstance`, `instanceLockFile` (`api.lock`, next to it).
- `src/app` (`hocket-headless`): `--list-devices`, `--check <folder>`, `--play <folder> [--driver] [--device] [--watch] [--user]`, `--version` (with the API build).
  - `--play` takes `api.lock` first and holds it until it exits, serves the API, writes the token file, and calls `checkIdle()` from its 50 ms timer. Exceptions in that timer are printed, not fatal.
  - `Console_windows.cpp` is the console's platform layer: UTF-8 output, Ctrl+C, and QuickEdit off until exit.
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
  - The checks: the tests (with the engine stress test's numbers); `--play` loops; the MCP path as Claude Code runs it; `hocket-stress`; a clean Ctrl+C (exit code 0, the `stopped` line, the token file removed) with the driver's xrun count, and the late callbacks as a note.
  - What it makes of a run's end is `Get-StopResults` in `AcceptanceChecks.ps1`, tested by `tests/acceptance`.
  - `-Driver`/`-Device` go to `--play`. `-FreshClone` (with `-CloneFrom`) also clones and builds with CLAUDE.md's commands. `Send-CtrlC.ps1` stops `--play` the way Ctrl+C does.
- Tests (`ctest`, 11 tests, about 40 s, most of it the stress tests running in real time):
  - `tests/core` (`hocket_core_tests`): 89 cases. It links `hocket_platform` for `FileLock`, which holds a clip file the way an editor might.
  - `tests/engine` (`hocket_engine_tests`, own main, one shared Tracktion `Engine`): 23 cases, about 40 s.
    - Edit structure; offline renders (notes start where `.notes` says; `write_clip` and undo move them).
    - `PlaybackDevice` against a fake JUCE device type that a test can unplug; it never opens a real device.
    - Live playback through Tracktion's hosted audio device: looping, a mid-loop rewrite heard by the next pass, a sounding note swapped for another (the old one ends, the new one waits for the next pass), a held note not retriggered by a rewrite, plus a control showing that check catches a retrigger.
    - Three stress tests (`StressTests.cpp`, about 30 s) in a real-time harness. An `AudioThread` plays `FakeAudioDevice::playBlock` at a sound card's pace with real-time priority, timing each block, while the test thread is the message thread.
      - 200 comment-only rewrites at 20/s to a held note. Checked: no block over its 5.8 ms budget (Release only), and no gap, retrigger or click against the first, untouched pass. A dropped block, a whole block of silence, fails it too. A control damages a copy to show each check can fail.
      - 21 rewrites that move a note, with no block over budget, and the last rewrite is what plays.
      - `hocket-stress --vary`'s rewrites of the demo line (200, every other one an octave up), then an empty clip: silence afterwards, and no note left on in the MIDI reaching 4OSC. `MidiRecorder`, a test plugin put ahead of 4OSC, records that MIDI without allocating.
  - `tests/api` (`hocket_api_tests`): 50 cases. The handler driven directly (hello, the token and the build check, every tool, argument errors, interrupts on disconnect and idle with a fake clock, internal errors), the fingerprint, `TextChanges`, the token file, the instance lock and other platform functions, and end-to-end tests: an IXWebSocket client talking to the server on 127.0.0.1 (ports from 46300), with the test thread as the message thread.
  - `tests/mcp` (`hocket_mcp_tests`): 10 cases. The bridge against a real `Server` and `Handler` on a thread of their own (`FakeHocket`, ports from 46320, token file in a non-ASCII temp folder). Covers: tools listed without Hocket; not running (no file, a stale file, nothing listening); write, undo and history as the client's own author; names and labels; a restart (same session, note once); another build; a timeout with a late response dropped; protocol errors.
  - `hocket_mcp_stdio` (CTest, `tests/mcp/StdioTest.cmake`): runs `hocket-mcp.exe` on piped stdin with a byte order mark, a blank line and a CRLF line. Checks one LF-terminated JSON reply per request and nothing else on stdout, and that the non-ASCII `--token-file` and `--label` arrive intact.
  - `tests/stress` (`hocket_stress_tests`): 5 cases. The rewrites' text (steady and varied); a run against `FakeHocket` (ports from 46340): paced, all applied, the clip put back, the history; errors before starting; the summary. `hocket_stress_not_running` (CTest) runs the program with a non-ASCII `--token-file` that doesn't exist.
  - `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder.
  - `tests/acceptance` (`acceptance_checks`, CTest, Windows PowerShell): `Get-StopResults` on made-up `--play` logs and exit codes: clean stops, xruns, a crash before and after the `stopped` line, a `--play` that stopped early or ignored Ctrl+C, a token file left behind.
  - `tests/common`: `TemporaryFolder.h` (non-ASCII temp folders) and `FakeHocket.h` (a bus, handler and API server on a thread of their own, plus a token file: a running Hocket, as far as a client can tell).
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.

## How Milestone 0 was verified
- **A fresh clone**, on 2026-11-06: `Run-Acceptance.ps1 -FreshClone -Driver ASIO -Device "FL Studio ASIO"` cloned GitHub's `main` at `d0a1d31`, with Tracktion from Hocket's fork. The clone, submodules, configure, a clean Release build and its 11 tests passed in 4.7 min, then every silent check over FL Studio ASIO. The engine stress test dropped no blocks.
- **By ear, with Lee**, on the laptop over FL Studio ASIO, on 2026-11-04 and 2026-11-06:
  - The demo loops. Claude's `write_clip` from Claude Code was heard on the next pass with no click, gap or stutter, and `undo` brought the demo line back.
  - `--watch`: Lee's save played as Lee's entry, with the warning naming the change of Claude's it replaced. A save with a mistake changed nothing. Claude's `undo` refused Lee's change, and `history` listed both of them.
  - `hocket-stress`: three plain runs with nothing heard, and `--vary` (after the stuck-note fix) ending cleanly, with nothing left ringing.
  - Ctrl+C: no audio callback took longer than its block. None of the laptop's drivers counts xruns (SPEC §9).
- **Drivers:** ASIO4ALL buzzed on the Magni while FL Studio ASIO played cleanly (Loose ends). Device names as `--list-devices` prints them: `FL Studio ASIO`, `ASIO4ALL v2` and `Realtek ASIO` under ASIO; the Magni under Windows Audio is `Speakers (Magni Mesh Dac)`.

**Set up on Lee's laptop:** `hocket-mcp.exe` is installed in `%LOCALAPPDATA%\Hocket\bin` and registered in Claude Code as `hocket` at user scope (API build `c4f35c9e10c86405`). Every Claude Code session gets the tools; they say "Hocket isn't running" until `--play` runs. After a change to the tools or the API, run CLAUDE.md's install command again and restart Claude Code sessions.

**To do when Lee sets up Claude Desktop** (not started; don't drop this):
1. Register the installed `hocket-mcp.exe` in Claude Desktop's config. It should be `%APPDATA%\Claude\claude_desktop_config.json`, under `"mcpServers": { "hocket": { "command": "<path to hocket-mcp.exe>" } }`; check against Claude Desktop's own docs. Then restart Claude Desktop.
2. Find the `clientInfo` name it sends. hocket-mcp logs `client '<name>' <version>, MCP <version>` to stderr on `initialize`, and Claude Desktop keeps servers' stderr in its MCP logs (expected in `%APPDATA%\Claude\logs\`, e.g. `mcp-server-hocket.log`). `--play`'s `API: ... connected` line shows the name Hocket ends up using.
3. Map that name to "Claude" in `mcp::authorName` (`src/mcp/Bridge.cpp`). Add a check to "names and labels are made to fit hello" in `tests/mcp/BridgeTests.cpp`. Update SPEC §6.3's `client` line and the "Other MCP clients" section of `docs/connecting-claude-code.md`. Rebuild and reinstall.
4. Check the label too. Claude Desktop isn't started in a project folder, so the working folder's name may mean nothing (an app or system folder). If so, pass `--label` in its config, or pick a better default for clients that don't start the bridge in a project folder.

## Test hardware
- **Laptop** (main development machine): Magni Mesh DAC, used at the desk. It has no ASIO driver of its own. ASIO4ALL put a buzz over the bass (Loose ends); FL Studio ASIO works, so Milestone 0's ASIO checks (steps 5 and 8) use it, here.
- **Desktop**: Focusrite Scarlett 2i2 with Focusrite's native ASIO driver and mic inputs. It'll be the test interface for v1 recording.

## Next: plan v1 (SPEC §10.1)
Milestone 0 is done. Next is a plan for v1, agreed with Lee before any of it starts, the GUI included. To settle in that plan:
- Which loose ends go into v1's first steps: the two deferred review items, `--play` saying when playback stops by itself, text selection while it runs, choosing the sample rate and buffer size (SPEC §9), and device changes during playback (SPEC §11).
- Claude Desktop, when Lee sets it up (above).

## Code review (2026-10-31)
The whole of Milestone 0, reviewed against SPEC.md and CLAUDE.md; the findings are in this file's history (`git log -- PROGRESS.md`). Everything is fixed, each with a test, and merged into `main` on 2026-10-31 (from branch `m0-review-fixes`), except two Lee deferred, now in Loose ends: the idle timeout and notifications, and closing the console window. Looked for and not found:
- Audio thread: no Hocket code runs on it. `EditSync` changes the Edit on the message thread; Tracktion builds the new graph there and destroys the retired one there too, after its pool threads let go (`LockFreeMultiThreadedNodePlayer::postNewGraph`).
- Thread safety: IXWebSocket's threads only post to the message thread; the bus, handler, the server's connection map and `EditSync` are only used there; `Client`'s shared state is behind its mutex; the Ctrl+C flag is atomic.

## Loose ends
- **Deferred from the review (2026-10-31):**
  - The idle timeout doesn't count notifications: `Handler::handleRequest` returns for one before it sets `lastRequest`, while SPEC §3.1 says "last request of any kind". No effect yet: hocket-mcp forwards none. Fix: set it first, or change SPEC's wording.
  - Closing the console window isn't a clean stop: `handleConsoleControl` (`Console_windows.cpp`) returns FALSE for `CTRL_CLOSE_EVENT`, so the device isn't closed and the token file stays (the stale-file check copes). Fix: handle it like Ctrl+C; Windows gives the handler about 5 s.
- **ASIO4ALL v2 on the Magni buzzes (2026-11-04).** Sharp, buzzy "NES helicopter" sounds came with the bass line. Windows Audio and FL Studio ASIO, on the same DAC, sounded fine. The cause isn't known.
  - Hocket doesn't choose a sample rate or buffer size. It opens the device with whatever the driver or Tracktion's saved settings give, and prints both in its `Playing on` line. With no saved rate, JUCE takes the driver's current rate.
  - The log points at the sample rate, not the buffer. Both ASIO drivers ran at 44100 Hz with 512 samples, but Windows runs the Magni at 48000 Hz. FL Studio ASIO goes through Windows' audio engine, which converts the rate. ASIO4ALL drives the device directly, by kernel streaming, so it needs the DAC itself at 44.1 kHz while Windows holds it at 48 kHz.
  - The test: set the Magni to 44100 Hz in Windows' Sound settings (the device's Properties, then Advanced) and try ASIO4ALL again. Or turn on "Always Resample 44.1kHz <-> 48kHz" in ASIO4ALL's advanced panel.
  - `--play` has no option for the sample rate or buffer size. `--sample-rate` and `--buffer-size` would make this testable from the command line.
  - It isn't in Milestone 0's way, since FL Studio ASIO works. But plenty of people only have ASIO4ALL, and if Hocket opens it with settings that don't work, picking better ones is Hocket's job.
- **No text selection while `--play` runs (2026-11-04).** Lee couldn't select any text in the terminal, so had to stop `--play` to copy its log. The cause is the review's fix #10, which turns QuickEdit off. That keeps a selection from pausing console output (in the classic console, a selection blocks the writer, and so the message thread). Without QuickEdit, though, the classic console has no mouse selection, and Windows Terminal hands the mouse to the program (Shift+drag still selects there).
  - The fix: write console output from a thread of its own, through a queue, so a selection only pauses that thread. Then leave QuickEdit as the user had it.
- **Small things from Lee's first run (2026-11-04):**
  - `--device "Magni Mesh Dac"` was refused, since the name is `Speakers (Magni Mesh Dac)`. The error listed the right name, so it worked as designed. When exactly one device contains the given text, the error could say "did you mean...".
  - `AcceptanceChecks.ps1`'s Xruns NOTE says "ASIO drivers usually do" report them; ASIO4ALL and FL Studio ASIO don't (item 8 above). Fix the wording, and its test if one matches the text.
- **Clip starts in `project.json` have no upper bound.** A clip is at most 999 bars long, but one can start at bar 100000, which makes as long a loop. Only a hand-edited `project.json` can do it in Milestone 0.
- **Dropped blocks: fixed in Hocket's Tracktion fork (2026-11-06).** Tracktion now and then played a whole block of silence (256 samples, 5.8 ms: a click) while a clip was rewritten during playback, about once per 1,700 graph rebuilds. The submodule now points at `pastaplsok/tracktion_engine`, branch `bugfix/graph_rebuild_skips_audio_block`: upstream plus one commit (DEPENDENCIES.md).
  - **The cause:** `LockFreeMultiThreadedNodePlayer::prepareToPlay (node, oldGraph, ...)` calls `createThreads()` for every new graph, and that took `processMutex` even when the threads already existed. The audio thread's `process()` only try-locks `processMutex` and skips the whole block when that fails: no node runs, and the playhead moves on. The fix only takes the lock when there are threads to create.
    - Don't simply remove that call: `numThreadsToUse` starts at the core count minus one, so a `setNumThreads` with the same value returns early, and that call is what starts the pool's threads.
  - **How it was found** (temporary logging, since taken out): `MidiRecorder` saw edit time jump by exactly one block at each dropped block, so the plugins never ran there. The player's log showed `try_lock` failing on that block, right after a rebuild's `createThreads`.
  - **Checked:** the "hammer" (1,000 comment-only rewrites at 100/s to a held note) dropped 2 blocks in 20 runs on upstream, and none in 60 runs with the fix, with the pool's threads running. The stress test's dropped-block check fails again instead of warning. Side effect: rebuilds no longer wait for the current block, so they don't bunch up (about 400 swaps per 1,000 rewrites before, about 1,000 after).
  - **Kept long-term.** The fix stays in the fork, not reported upstream (Known upstream issues, below). When moving to a newer Tracktion, follow DEPENDENCIES.md's "Updating Tracktion": check whether upstream fixed it on its own, and if not, rebase the one commit in the fork.
  - **One unexplained failure:** on 2026-10-30, a Debug run of the two stress tests failed once, right after a full Debug `ctest`. My filter hid the error, so which check failed isn't known. It didn't happen again in 23 Debug runs since: 11 of the stress tests alone, and 12 of the whole engine suite. If it shows up again, keep the whole output.
- **Tracktion stops playback when it rebuilds its device lists.** Applying a new MIDI device list (or an audio device change) releases every playback context, which stops the playhead, and the transport then stops itself. `--play` works around the one that always happens: Tracktion's first MIDI scan otherwise lands 4 s after startup (see Gotchas). A MIDI device plugged in during playback would still stop it. Milestone 0 can live with that; v1, with MIDI input and device hot-plug, needs an answer (keep playing, or restart).
  - It happened over ASIO on 2026-11-04. During the first of Lee's ASIO4ALL runs, Lee changed the output device in ASIO4ALL's panel, and Ctrl+C later found playback "already stopped". That fits: a driver asks for a reset when its settings change (`kAsioResetRequest`), and JUCE then restarts the device. Not reproduced on purpose yet.
  - `--play` prints nothing when playback stops by itself. It should, at least, so a stop isn't mistaken for silence or a crash.
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
- **Render speed.** An offline render of a few seconds takes about 1.6 s (Release), and each live test about 0.5 s. That's most of the engine tests' time outside the stress tests (about 15 s of their 35). The cost looks like 4OSC building its wavetables each time it initialises ("Generating waves" in the Debug log). It doesn't matter for Milestone 0, but it will for `render_section` and the analysis tools (SPEC §7).
- **Debug engine tests:** no JUCE assertions fire and JUCE's leak detector is silent, but the MSVC CRT reports two to four 16-byte blocks at exit, even when no test case runs. Not chased.
- **`--watch`** compares each file's modification time and size. A same-size rewrite within the file system's timestamp resolution could be missed, which is unlikely in practice.
- **Saving to disk:** `project::write()` produces every file in memory; writing them to the folder (atomically, plus SPEC §6.1's external-change detection) isn't built. Milestone 0 doesn't need it.
- **Tracktion pin:** upstream `develop` at 3.5.0 is untagged, and the submodule is Hocket's fork with one fix on top (Loose ends: dropped blocks). Move to the 3.5.0 tag when it exists, following DEPENDENCIES.md's "Updating Tracktion". It covers the fork's commit and Hocket's two workarounds (the first MIDI scan, and `setUsesProxy (false)`), each kept until upstream has its own fix, and moving the JUCE pin to match.
- **Known upstream issues (not reported, by Lee's decision, 2026-11-06).** Tracktion takes bug reports only on the JUCE forum, whose community guidelines don't allow content generated by AI ([policy](https://forum.juce.com/t/new-policy-no-content-generated-by-artificial-intelligence/66362)). Hocket works around or fixes each one itself instead.
  - The three Tracktion reports Claude drafted (the first MIDI scan, the dropped block, and `MidiNode`) stay unposted in Lee's `Documents\Hocket\upstream`, untouched, in case Lee decides otherwise. Anything posted would have to be in Lee's own words, per that policy.
  - Each time Tracktion is updated, DEPENDENCIES.md's "Updating Tracktion" checks whether the first three Tracktion items below are still needed.
  - **Tracktion, the dropped block:** `LockFreeMultiThreadedNodePlayer::prepareToPlay` calls `createThreads()`, which takes `processMutex`, for every graph rebuild, so the audio thread's `try_lock` sometimes fails and a whole block plays as silence. Fixed in Hocket's fork, kept long-term (Loose ends: dropped blocks).
  - **Tracktion, the first MIDI scan:** `DeviceManager::initialise()` calls `rescanMidiDeviceList()` (a 5 ms timer) and then `restartMidiCheckTimer()`, which replaces it with the 4 s rescan timer. So the first MIDI scan lands 4 s after startup and stops anything already playing. Workaround: `--play` runs the scan itself before playing (`src/app/Main.cpp`; Gotchas).
  - **Tracktion, `MidiNode`'s notes:** `MidiNode` (`tracktion_MidiNode.cpp`), which plays MIDI clips that use a proxy sequence (the default), keeps no note state across graph swaps.
    - `prepareToPlay` only checks whether the old graph had the node (54-60).
    - The new node then sends the new sequence's note-offs unchecked (171-197), and its loop-end note-offs come from that sequence too (201-205).
    - So a rewrite that changes a sounding note leaves the old one on for good. `LoopingMidiNode` gets this right.
    - Workaround: `EditSync` calls `setUsesProxy (false)` on each clip, so clips play through `LoopingMidiNode`. Repro: `hocket_engine_tests --test-case="stress: rewrites that change*"` with that call taken out.
  - **Tracktion, 4OSC allocates on the audio thread:** it builds a `juce::MidiBuffer` for every 32-sample sub-block (`FourOscPlugin::applyToBuffer`, `tracktion_FourOscPlugin.cpp:1484`), and `addEvent` allocates whenever a sub-block has MIDI. Noticed while chasing the stuck notes; no glitch traced to it. 4OSC is Milestone 0's stand-in synth; Hocket's own comes in v1.
  - **Tracktion, the version string:** `Engine::getVersion()` is hardcoded to "v3.1.0"; we read `VERSION.md` instead.
  - **IXWebSocket:** its server sets `SO_REUSEADDR` unconditionally (`IXSocketServer.cpp:135`), so on Windows a second server can bind a port another is listening on. `listen()` is non-virtual and the socket private, so `SO_EXCLUSIVEADDRUSE` can't be set without forking. Also, its server sends block with no timeout by default (`kSendTimeoutSeconds = -1`); `Server.cpp` sets 2 s.

## Gotchas picked up so far
- **Line endings.** This machine has `core.autocrlf=true`. `.gitattributes` keeps `*.notes`, `*.auto` and `project.json` LF; add any new byte-compared fixture type there. In the Bash tool, `grep $'\r'` reports every line; count CRs with `tr -cd '\r' < file | wc -c`.
- **Don't name a helper `quoted`.** Argument-dependent lookup finds `std::quoted` instead (error C2676). The code uses `singleQuoted` / `doubleQuoted`.
- **nlohmann/json** is built with implicit conversions off: write `get<T>()`.
- **Property tests against the bus** must canonicalize their reference copy with `*notes::parse (text).clip`, because the bus stores the parsed clip (spellings filled in), not the text. The parsed clip isn't canonical, though: notes and comments stay where the text had them. To ask whether two clips are the same, compare `notes::write` of each, as the bus does for "changes nothing".
- **doctest has no `GENERATE`** (that's Catch2): loop over a table, with `INFO` for the case.
- **Windows PowerShell 5.1 reads a `.ps1` without a byte order mark as ANSI**, so keep scripts ASCII (`tests/acceptance` builds its non-ASCII folder name from `[char]` codes). And an expression can't continue onto a line that starts with `+`: build the string first.
- **Build time.** JUCE and Tracktion compile once into `hocket_juce`. A clean Release build takes 1 to 2.5 minutes here (2 min 21 s in a fresh clone on 2026-10-30). A fresh clone's time is mostly the submodule download: 21 minutes that evening, against about 4 minutes in all before.
- **Tracktion's first MIDI scan.** Before playing, call `engine.getDeviceManager().rescanMidiDeviceList()` and run the message loop briefly (`runDispatchLoopUntil (100)`), as `--play` does. Otherwise the scan lands 4 s after startup and stops playback. The engine tests never see this: the hosted device has `useMidiDevices = false`. A Tracktion bug, not reported (Known upstream issues); whether it's still needed is checked on every Tracktion update (DEPENDENCIES.md).
- **`JUCE_MODAL_LOOPS_PERMITTED=1`** (set on `hocket_juce`) makes `MessageManager::runDispatchLoopUntil` available. `runDispatchLoop` can only run once per process: after `stopDispatchLoop` it returns at once.
- **One Tracktion `Engine` at a time.** The engine tests share `testing::testEngine()`, which never opens a real device and keeps its settings in a temp folder. A `FakeAudioDevice` must be opened before the `EditSync` whose Edit plays on it.
- **Live tests and graph rebuilds.** Tracktion applies a change to the playback graph on a 1 ms message-thread timer. In single-threaded tests, call `edit.dispatchPendingUpdatesSynchronously()` to apply it at an exact point.
- **4OSC isn't deterministic** from run to run (oscillator phase), so compare loudness between runs, not samples.
- **The real-time harness** (`tests/engine/StressTests.cpp`). An `AudioThread` (a JUCE real-time thread) calls `FakeAudioDevice::playBlock` at a sound card's pace, while the test thread runs `runDispatchLoopUntil (1)` and sends commands; it behaves like the app.
  - `playBlock` renders into a buffer allocated up front: JUCE's `AudioBuffer::setSize` reallocates whenever it grows, even with `avoidReallocating` (that only helps when shrinking), so growing `output()` on every block would swamp the timing.
- **Engine tests that run the message loop** receive whatever Tracktion has posted before, and handling a device update stops the transport. A test that ran after another test's device was closed stopped playing within two blocks. `FakeAudioDevice` now handles pending messages when it opens and closes.
- **Tracktion has two MIDI clip nodes.** A clip that can use a proxy sequence (`MidiClip::canUseProxy()`, true by default) plays through `MidiNode`, which keeps no note state across graph swaps, so a rewrite that changes a sounding note leaves it stuck. With `setUsesProxy (false)` it plays through `LoopingMidiNode`, which hands its sounding notes to the next graph and ends the ones the rewrite took away (`createNodeForMidiClip`, `tracktion_EditNodeBuilder.cpp:676`). `EditSync` turns the proxy off for that reason, until upstream's `MidiNode` hands its notes over too (checked on every Tracktion update, DEPENDENCIES.md). Read the node that actually plays before reasoning about Tracktion's MIDI.
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
