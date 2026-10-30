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
| 6 | WebSocket API on 127.0.0.1 + token file; interrupted-transaction handling | ⏭ Next |
| 7 | `hocket-mcp` stdio bridge + doc on connecting Claude Code | ⬜ |
| 8 | Stress test, acceptance run, SPEC updates | ⬜ |

**Milestone 0 is done when:**
- a fresh clone builds with the commands in CLAUDE.md;
- the demo project loops over ASIO;
- from Claude Code, `write_clip` is heard on the next loop with no glitch, `undo` restores the old clip, and `history` shows who made each change;
- 200 rewrites at 20/sec during playback cause zero xruns (where the driver reports them) and no audible dropouts;
- all tests pass.

## What exists
- `src/core` (`hocket_core`, no JUCE): `notes/` (`.notes` model, parser, canonical writer), `project/` (`project.json` model, reader, writer; `ClipFileWatcher` for `--watch`), `commands/CommandBus` (the only way to change a `project::Project`; also routes transport commands to the engine), `Paths` (UTF-8 ⇄ path, `readTextFile`).
- `src/engine` (`hocket_engine`): `EditSync` builds a Tracktion `Edit` from the bus's project, rewrites a Tracktion clip on each `ClipContentChanged`, and is the bus's `Transport` (play loops the whole song).
- `src/app` (`hocket-headless`): `--list-devices`, `--check <folder>`, `--play <folder> [--driver] [--device] [--watch]`, `--version`. `Console_windows.cpp` is the platform layer (UTF-8 console, wide command line, Ctrl+C).
- Tests (`ctest`, about 10 s):
  - `tests/core` (`hocket_core_tests`): 69 cases.
  - `tests/engine` (`hocket_engine_tests`, own main, one shared Tracktion `Engine`): 14 cases. Edit structure; offline renders (notes start where `.notes` says; `write_clip` and undo move them); live playback through Tracktion's hosted audio device (looping, a mid-loop rewrite heard by the next pass, a held note not retriggered by a rewrite, plus a control showing that check catches a retrigger).
  - `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder.
  - `tests/common/TemporaryFolder.h`: non-ASCII temp folders for both.
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.

## Still open in step 5: the by-ear check over ASIO
Not done yet: it needs Lee at the laptop with the Magni. `--play` has only been run silently here (an empty clip over WASAPI): the device opens, it loops, `--watch` applies saves (errors print as `file:line:column`), and Ctrl+C stops cleanly.
1. `hocket-headless.exe --list-devices`, and check the Magni shows up under Windows Audio.
2. `hocket-headless.exe --play examples\m0-demo --driver ASIO --device "ASIO4ALL v2" --watch`, with ASIO4ALL routed to the Magni in its panel.
3. While it loops, edit `examples\m0-demo\clips\bass.notes` and save. Listen for the change landing by the next pass, with no click, gap or stutter. Try a save with a mistake in it; it should print the error and keep playing the old clip.
4. Ctrl+C. Note whether it prints an xrun count (ASIO drivers usually report one; WASAPI doesn't).
5. Revert `bass.notes` afterwards (`git checkout examples/m0-demo/clips/bass.notes`).

## Test hardware
- **Laptop** (main development machine): Magni Mesh DAC, used at the desk. It has no ASIO driver of its own, so ASIO goes through ASIO4ALL. Milestone 0's ASIO checks (steps 5 and 8) run here.
- **Desktop**: Focusrite Scarlett 2i2 with Focusrite's native ASIO driver and mic inputs. It'll be the test interface for v1 recording.

## Next: step 6 — WebSocket API
- **Interrupted transactions** (SPEC §3.1). When the client that opened a transaction disconnects, the API commits it as "(interrupted)" (`wrote bridge (interrupted)`, or `wrote bridge (part 2, interrupted)` after a split), keeping its author. An empty one just closes. An idle timeout does the same for a client that's connected but silent; it counts from that client's last request and is minutes long (pick the value then).
  - Bus side: add something like `interruptTransaction (author)` next to `commitTransaction` in `CommandBus.cpp`, which already builds the "(part N)" names. The API calls it on disconnect and from a message-thread timer.
  - Tests: edits survive a disconnect, the entry is labeled and undoable, an empty transaction leaves no entry, a split one gets "(part 2, interrupted)", and the timeout fires only after inactivity.
- **Threading.** `CommandBus` isn't thread-safe and must only be used on the message thread (SPEC §3.2). IXWebSocket calls back on its own threads, so every request has to be posted to the message thread and its reply sent back from there. `--play` already runs the message loop (`PlayLoop` in `Main.cpp` is a message-thread timer); the API server lives alongside it.
- **`read_clip`** is a query, not a command; it comes with the API. The "unknown clip ID" message in `CommandBus.cpp` names `write_clip` and will need generalizing.
- **Transport over the API.** `Play` and `Stop` are already bus commands (`commands::Transport`, implemented by `EditSync`). The MCP `transport` tool maps onto them. Seek and loop on/off aren't built; add them if the tool needs them.
- **Author identity** (SPEC §3.1). Authors are compared by identity: name + session ID. `hocket-mcp` generates a session ID at startup and keeps it across reconnects to the DAW, so two Claude sessions are different authors, and a client that reconnects is still the author of its own entries, including an interrupted transaction. Display names stay friendly ("Claude"), optionally with a label the client provides.
  - Bus side: `CommandBus` takes authors as plain strings today (`execute`, `undo`, `redo`, the transaction calls, `HistoryEntry::author`). Replace them with an author type that compares by identity and keeps the display name and label for messages. Everything that compares authors switches over: the undo check, transaction ownership, and splitting a transaction when another author edits. The GUI user gets one fixed identity.
  - **The human's display name becomes a setting** as part of this. Until then `hocket-headless` sends its own commands (play, stop, `--watch` saves) as `"user"` (`localUser` in `Main.cpp`).
  - Messages must still say who, even when the names match. For example: `was made by Claude in another session, not this one`, or with labels, `was made by Claude (drums), not Claude (mixing)`.
  - Still to decide: how the bridge hands the session ID and label to the API (presumably in its first message, alongside the token), and how a client sets its label.

## Loose ends
- **Tracktion stops playback when it rebuilds its device lists.** Applying a new MIDI device list (or an audio device change) releases every playback context, which stops the playhead, and the transport then stops itself. `--play` works around the one that always happens: Tracktion's first MIDI scan otherwise lands 4 s after startup (see Gotchas). A MIDI device plugged in during playback would still stop it. Milestone 0 can live with that; v1, with MIDI input and device hot-plug, needs an answer (keep playing, or restart).
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
  - `DeviceManager::initialise()` calls `rescanMidiDeviceList()` (a 5 ms timer) and then `restartMidiCheckTimer()`, which replaces it with the 4 s rescan timer. So the first MIDI scan lands 4 s after startup and stops anything already playing.

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
