# Progress

Status board for the current milestone. Read it at the start of a session; update it at the end of each step. Keep it current, not historical — git log is the history.

## v1 (SPEC §10.1)
Planned with Lee on 2026-11-06. What each milestone covers is in SPEC §10.1; each one's steps are planned here when it starts.

| # | Milestone | Status |
|---|---|---|
| 0 | Headless proof (SPEC §10.0) | ✅ Done, 2026-11-06. Step record: `git log -- PROGRESS.md` |
| 1 | A song, not a loop (no GUI yet) | Next; steps below |
| 2 | The window | Not started |
| 3 | Piano roll and MIDI, including MIDI recording | Not started. Needs the desktop's toolchain (Test hardware) |
| 4 | Instruments: Hocket's synth, the full sampler, all three kits | Not started |
| 5 | Effects and mixer | Not started |
| 6 | Automation | Not started |
| 7 | Audio and recording | Not started |
| 8 | Render, export, analysis | Not started |
| 9 | Finishing v1 | Not started |

## Milestone 1 — a song, not a loop
Still headless: `hocket-headless`, driven through the API. The GUI is Milestone 2.

| # | Step | Status |
|---|---|---|
| 1 | Device options and Milestone 0's loose ends | Done (2026-11-12); two checks wait for the Magni (Loose ends) |
| 2 | The format grows to a song: linked clips, time signatures, track mix settings, `hocket:synth` | Next |
| 3 | Drum tracks in the format: drum maps, drum names in `.notes`, `hocket:drum-sampler` | Not started |
| 4 | Commands for the song's structure, with undo and gesture coalescing | Not started |
| 5 | Sync: tracks and clips added, moved and removed during playback | Not started |
| 6 | Sync: mix settings, tempo, time signature, seek and loop region | Not started |
| 7 | Hocket's drum sampler, the first Hocket device, and how kits work | Not started |
| 8 | The first bundled drum kit | Not started |
| 9 | Saving, the recovery copy, and changes made outside Hocket | Not started |
| 10 | MCP tools for all of it | Not started |
| 11 | Demo song, acceptance run, SPEC updates | Not started |

**Milestone 1 is done when:**
- starting from an empty project, Claude builds a song with at least four tracks, drums among them, through the API while Lee listens, with no glitch;
- Lee's and Claude's changes are in one history, and undo works across structural changes;
- save, load and save again gives byte-identical files, and a crash leaves a recovery copy;
- each new kind of change to the Edit has a real-time stress test, and all tests pass;
- a fresh clone builds and passes `Run-Acceptance.ps1`.

### The steps
**1. Device options and Milestone 0's loose ends**
- Done, with tests:
  - `--play --sample-rate <Hz> --buffer-size <samples>` (SPEC §9). A value the device doesn't offer is refused before anything changes, and so is a driver that carries on at another rate or size.
  - `--list-devices --driver <type>` or `--device <name>` also lists those devices' sample rates and buffer sizes. Plain `--list-devices` still loads no driver: creating a JUCE ASIO device loads and initialises its driver (`ASIOAudioIODevice`'s constructor calls `openDevice()`), which could disturb another program using it.
  - `--play` says when playback stops by itself, and what changed on the device if anything did (`EditSync::stoppedByItself`).
  - The idle timeout counts notifications. Closing the console window stops `--play` cleanly: the handler sets the Ctrl+C flag and waits, and Windows ends the process when `main` returns, or after about 5 s.
  - "Did you mean" for a driver or device when exactly one name contains the text given, ignoring case. The acceptance script's xrun note has the right wording.
  - Silent checks on the laptop: Windows Audio offers the Magni at 48000 Hz only, and exclusive mode offers 44100 to 192000 Hz. The suggestions work against the real device names.
  - A driver's own error when it won't start ("Device didn't start correctly") is followed by what to try: the driver's settings, or another device.
- Checked with Lee on 2026-11-12, on the laptop with headphones on the Realtek (the Magni was elsewhere), with a copy of the demo whose clip is empty:
  - `--list-devices --driver ASIO` lists all three drivers with their rates and sizes. ASIO4ALL: 8000 to 192000 Hz, 64 to 2048 samples in steps of 16. FL Studio ASIO: 22050 to 192000 Hz, 256 to 4096 in steps of 16. Realtek ASIO: 44100 to 192000 Hz, 512 samples only, yet it ran at 544 when opened at 44100 Hz.
  - Changing a setting in ASIO4ALL's panel during `--play` printed `Playback stopped by itself`, blaming Tracktion's device lists: the driver reset the device and reopened it as it was.
  - Closing the console window removed `api.json`.
  - Realtek ASIO opened and stopped cleanly. FL Studio ASIO wouldn't start ("Device didn't start correctly"), with or without `--sample-rate` (Loose ends).
- Left for the Magni (Loose ends): the ASIO4ALL buzz at 48000 Hz, and whether FL Studio ASIO opens there.
- Left out: text selection while `--play` runs. The GUI replaces `--play` for everyday use.

**2. The format grows to a song** (core; `--check`; round-trip, error, property and fuzz tests)
- Linked clips (SPEC §4.4): several clips naming one `.notes` file. The model keeps each file's notes once, keyed by file, and clips refer to it; the writer writes each file once.
- Time signatures other than 4/4, one per song. To decide with Lee, since it's the format: which are allowed, and what a beat is in 6/8 or 7/8, which `.notes` positions and `+ticks` count (SPEC §11).
- Each track's volume, pan, mute and solo in `project.json`, in real units (SPEC §4.1, rule 3). To decide: how they're written (`"volume": "-6dB"`, and pan how?) and whether defaults are written out.
- Tracks with no device. An upper bound on clip starts (Loose ends), so a song can't be thousands of bars long either.
- The synth's type becomes `hocket:synth`, with 4OSC standing in until Milestone 4 (SPEC §8.1), so songs written now keep working then. It has no parameters yet.

**3. Drum tracks in the format** (SPEC §5.2)
- A track's drum map in `project.json`, and drum names in `.notes`, read and written through the track's map, with note names as the fallback. General MIDI names by default.
- `hocket:drum-sampler`, with the name of a kit: `{"id", "type": "hocket:drum-sampler", "kit": "<name>"}`. The model only checks the name against the kits it's given; step 7 says where kits come from.
- To decide: how a kit's piece names and the track's drum map relate (the map comes from the kit, or kits put their pieces on General MIDI notes and use the default map); and what happens when one file is linked into tracks with different maps (each reads it with its own map, or it's an error).
- The same tests as step 2, plus drum names round-tripping through `write_clip`.

**4. Commands for the song's structure** (the bus)
- Tracks: add, remove, rename, reorder, and set the instrument (`hocket:synth`, or `hocket:drum-sampler` with a kit). Clips: add one (with a new file, or linked to an existing one), move it (in time, or to another track), remove it, and make a linked clip unique. Also: tempo, time signature, a track's mix settings, and its drum map.
- Transport: seek, loop region, loop on and off. Not history entries (SPEC §3.1).
- Every edit has an inverse, so undo, redo, rollback and the split of another author's transaction work as they do for `write_clip`. History descriptions say what changed ("added track trk-drums", "moved clp-bass-2 to 9.1").
- Gesture coalescing (SPEC §3.1), with the first continuous settings, volume and pan: a client marks where a gesture starts and ends, and its changes become one entry. Built and tested here; the GUI uses it in Milestone 2.
- `Change` grows from `ClipContentChanged` to what the sync, and later the GUI, need to know.
- Tests: validation messages written for an LLM reader; undo and redo of each command; property tests on random command sequences (undoing everything gives the original back, and redoing everything gives the same project again).

**5. Sync: tracks and clips**
- `EditSync` brings the Edit in line with the model after each change, matching tracks, devices and clips by ID and touching only what changed. (It asserts today that clips are never added or removed.)
- Offline renders: tracks and clips added, moved, removed, linked, and undone.
- Live tests: a change to one track doesn't stop or retrigger a held note on another, and a clip moved or removed ends its sounding notes.
- A stress test for each kind of change: clips moved, added and removed, and tracks added and removed, 20 a second during playback. Each has Milestone 0's checks: no block over budget, no dropped block, gap, click, retriggered note or stuck note.

**6. Sync: mix settings, tempo, time signature, transport**
- Volume and pan through Tracktion's volume plugin, smoothed so that a jump doesn't click; mute and solo.
- Tempo and time-signature changes during playback: the playhead keeps its place in bars, and held notes aren't retriggered.
- Transport: seek, loop region, loop on and off.
- Stress tests: volume and pan changed 20 a second with no click, mute toggled, tempo changed.

**7. Hocket's drum sampler, and how kits work** (SPEC §8.1, §8.4)
- Hocket's first device, a Tracktion plugin type of its own (`PluginManager::createBuiltInType`). It sets the pattern the synth and effects follow in Milestones 4 and 5.
- One-shot pieces; velocity sets the level. Voices are allocated up front, samples are loaded on the message thread and handed over without locks, and nothing on the audio thread locks or allocates.
- Not Tracktion's `SamplerPlugin`, which locks and allocates on the audio thread (Known upstream issues).
- Kits: in the repository under `assets/kits/<name>/`, each with a small manifest (piece names, notes, files, levels), and installed next to the programs. A project names its kit; nothing is copied into the project.
- Tests use generated samples, not a real kit: hits land where the clip says, at the right level; a drum clip rewritten 20 times a second plays with no glitch, on the real-time harness with `MidiRecorder`.

**8. The first bundled drum kit** (SPEC §8.4)
- One kit now, in the style Lee picks. All three (acoustic, electronic/808-style, breakbeat/jungle) are in by Milestone 4.
- Lee's rules: an explicit permissive license (CC0 or similar), nothing taken from copyrighted recordings, and short 16-bit one-shots. Each source and license goes in DEPENDENCIES.md, with the total size.
- For the electronic kit, synthesizing the one-shots ourselves avoids sourcing altogether. The generator would stay in the repository, and the samples would be released as CC0.
- Lee listens to it before it's committed.

**9. Saving, the recovery copy, and changes made outside Hocket** (SPEC §4.5, §6.1)
- `--new <folder>` makes an empty project.
- Save is a command any client can send, and not a history entry. Each file is written atomically, clip files before `project.json`. The bus knows whether anything is unsaved.
- To decide: what saving does with a `.notes` file that no clip uses any more.
- The recovery copy: a few seconds after changes stop, the project is written to `%LOCALAPPDATA%\Hocket\Recovery\`. It's removed after a save, or a clean exit with nothing unsaved. Loading a folder that has a newer recovery copy says so, and `--play --recover` loads the copy.
- Outside changes: save refuses to overwrite a file changed on disk since Hocket last read or wrote it, and names the file. `--watch` covers `project.json` too: a saved change becomes one command that replaces the project's structure, validated and undoable like any other.
- Tests on a non-ASCII folder: atomic writes; save, load and save again byte-identical; recovery; outside changes.

**10. MCP tools for all of it** (SPEC §6.4)
- Tools for steps 4 and 9: tracks, clip placement, tempo and time signature, transport with seek and loop, and `save`. Also `patch_clip`, `history`'s author filter, and a `get_project_overview` that shows mix settings, links, drum maps and kits.
- The set is designed at the start of the step, small and expressive as SPEC §6.4 asks, and written into §6.4.
- Descriptions, results and errors are written for an LLM reader. The fingerprint changes, so `hocket-mcp` must be installed again (CLAUDE.md).
- API tests for each tool; the bridge's and stress client's tests updated.

**11. Demo song, acceptance run, SPEC updates**
- The acceptance check itself: starting from `--new`, Claude builds a song with drums while Lee listens. It's saved as `examples/m1-demo`.
- `hocket-stress` and `Run-Acceptance.ps1` get a structural mode: clips moved and tracks muted during playback, silently, on clips left empty.
- SPEC gets what Milestone 1 taught. A fresh-clone run.

## What exists
- `src/core` (`hocket_core`, no JUCE):
  - `notes/`: the `.notes` model, parser and canonical writer. A clip is at most 999 bars (`maxLengthInBars`).
  - `project/`: the `project.json` model, reader, writer, and lookups like `findClip` and `songEnd`; `ClipFileWatcher` for `--watch`, which keeps trying a settled file it can't read yet, for up to 2 s.
  - `commands/CommandBus`: the only way to change a `project::Project`. Authors are `Author { name, session, label }`, compared by name + session. A write is a no-op when the clip's canonical text wouldn't change. `interruptTransaction`, `openTransaction`, `changesTo` (the entries that changed a clip); it also routes transport commands to the engine, and `isPlaying()` asks it.
  - `commands/WatchedSaves`: `--watch`'s saves as the person's `write_clip`, with a warning when one replaces a change made some other way since the file and clip last matched.
  - `Paths`: UTF-8 ⇄ path (`utf8FromPath` never throws: an unpaired UTF-16 surrogate becomes U+FFFD), `readTextFile`.
- `src/platform` (`hocket_platform`): `Platform.h` with `Platform_windows.cpp` / `Platform_posix.cpp`: the command line as UTF-8 (`utf8Arguments`, used by both programs), binary stdin/stdout, secure random bytes, process ID and program name, "is this program still running", `%LOCALAPPDATA%`, `FileLock` (one holder at a time; on Windows nothing else can even read the file), and the TCP port probe. The POSIX file has never been compiled.
- `src/engine` (`hocket_engine`):
  - `EditSync` builds a Tracktion `Edit` from the bus's project, rewrites a Tracktion clip on each `ClipContentChanged`, and is the bus's `Transport` (play loops the whole song). Its clips don't use Tracktion's proxy sequences, so they play through `LoopingMidiNode`, which ends the notes a rewrite takes away (Gotchas). `stoppedByItself()` tells a stop Tracktion made from one a transport command asked for.
  - `PlaybackDevice.h`:
    - `openDevice (DeviceChoice)`, from `--driver`, `--device`, `--sample-rate` and `--buffer-size`. It refuses a rate or size the device doesn't offer before changing anything, and catches a driver that carries on as it was. Errors suggest a name when exactly one contains the text given.
    - `listDevices` for `--list-devices`. Only the driver or device named has its rates and sizes listed, since that loads the driver. `formatNumbers` writes evenly spaced runs as ranges.
    - `PlaybackDevice`: what `--play` says about the device at the start and at Ctrl+C. It asks JUCE's device manager each time, since the manager deletes a device that disappears and opens another. The xrun report has the driver's count (where it keeps one) and JUCE's count of audio callbacks that took longer than their block.
- `src/api` (`hocket_api`, no JUCE): the WebSocket API (SPEC §6.3), its server and its client.
  - `Handler`: JSON-RPC in, reply out; `hello` with the token and the build check (error -32002 for another build); per-client state; interrupts on disconnect and in `checkIdle()`. It never throws: anything thrown while handling a request is answered with -32603.
  - `Protocol`: what both ends share: `protocolVersion` (bump it for changes to `hello` or the formats), `defaultIdleTimeout` (10 minutes), `hocketVersion()`, `buildFingerprint()` (FNV-1a of the protocol version and `toolDefinitions()`), and `makeSessionId()` for clients.
  - `Client`: a blocking IXWebSocket client, one request at a time; a late response to a timed-out request is dropped. Used by hocket-mcp and hocket-stress.
  - `Tools`: the 9 Milestone 0 tools, their definitions and argument checks.
  - `TextChanges`: write_clip's "stored differently" summary.
  - `Server`: IXWebSocket glue; posts every message to the message thread.
  - `TokenFile`: `api.json` read/write, `makeToken`, `runningInstance`, `instanceLockFile` (`api.lock`, next to it).
- `src/app` (`hocket-headless`): `--list-devices [--driver] [--device]`, `--check <folder>`, `--play <folder> [--driver] [--device] [--sample-rate] [--buffer-size] [--watch] [--user]`, `--version` (with the API build).
  - `--play` takes `api.lock` first and holds it until it exits, serves the API, writes the token file, and runs a 50 ms timer. The timer says when playback stops by itself and calls `checkIdle()`. Exceptions in it are printed, not fatal.
  - `Console_windows.cpp` is the console's platform layer: UTF-8 output, Ctrl+C, closing the window (stops like Ctrl+C), and QuickEdit off until exit.
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
- Tests (`ctest`, 11 tests, 40 to 60 s, most of it the stress tests running in real time):
  - `tests/core` (`hocket_core_tests`): 89 cases. It links `hocket_platform` for `FileLock`, which holds a clip file the way an editor might.
  - `tests/engine` (`hocket_engine_tests`, own main, one shared Tracktion `Engine`): 32 cases, about 40 s (the whole `ctest` took 44 s on 2026-11-12). It took 53 s once, on 2026-11-06, with the laptop busy (47% CPU load, 2.4 GB of memory free): the time went to the live tests, about 3.7 s each instead of 0.5 s.
    - Edit structure; offline renders (notes start where `.notes` says; `write_clip` and undo move them); a stop no command asked for.
    - `PlaybackDevice` against a fake JUCE device type that a test can unplug, and whose rates, sizes and stubbornness a test picks; it never opens a real device. Choosing rates and sizes, suggestions, `listDevices`, `formatNumbers`.
    - Live playback through Tracktion's hosted audio device: looping, a mid-loop rewrite heard by the next pass, a sounding note swapped for another (the old one ends, the new one waits for the next pass), a held note not retriggered by a rewrite, plus a control showing that check catches a retrigger.
    - Three stress tests (`StressTests.cpp`, about 30 s) in a real-time harness. An `AudioThread` plays `FakeAudioDevice::playBlock` at a sound card's pace with real-time priority, timing each block, while the test thread is the message thread.
      - 200 comment-only rewrites at 20/s to a held note. Checked: no block over its 5.8 ms budget (Release only), and no gap, retrigger or click against the first, untouched pass. A dropped block, a whole block of silence, fails it too. A control damages a copy to show each check can fail.
      - 21 rewrites that move a note, with no block over budget, and the last rewrite is what plays.
      - `hocket-stress --vary`'s rewrites of the demo line (200, every other one an octave up), then an empty clip: silence afterwards, and no note left on in the MIDI reaching 4OSC. `MidiRecorder`, a test plugin put ahead of 4OSC, records that MIDI without allocating.
  - `tests/api` (`hocket_api_tests`): 51 cases. The handler driven directly (hello, the token and the build check, every tool, argument errors, interrupts on disconnect and idle with a fake clock, notifications included, internal errors), the fingerprint, `TextChanges`, the token file, the instance lock and other platform functions, and end-to-end tests: an IXWebSocket client talking to the server on 127.0.0.1 (ports from 46300), with the test thread as the message thread.
  - `tests/mcp` (`hocket_mcp_tests`): 10 cases. The bridge against a real `Server` and `Handler` on a thread of their own (`FakeHocket`, ports from 46320, token file in a non-ASCII temp folder). Covers: tools listed without Hocket; not running (no file, a stale file, nothing listening); write, undo and history as the client's own author; names and labels; a restart (same session, note once); another build; a timeout with a late response dropped; protocol errors.
  - `hocket_mcp_stdio` (CTest, `tests/mcp/StdioTest.cmake`): runs `hocket-mcp.exe` on piped stdin with a byte order mark, a blank line and a CRLF line. Checks one LF-terminated JSON reply per request and nothing else on stdout, and that the non-ASCII `--token-file` and `--label` arrive intact.
  - `tests/stress` (`hocket_stress_tests`): 5 cases. The rewrites' text (steady and varied); a run against `FakeHocket` (ports from 46340): paced, all applied, the clip put back, the history; errors before starting; the summary. `hocket_stress_not_running` (CTest) runs the program with a non-ASCII `--token-file` that doesn't exist.
  - `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder.
  - `tests/acceptance` (`acceptance_checks`, CTest, Windows PowerShell): `Get-StopResults` on made-up `--play` logs and exit codes: clean stops, xruns, a crash before and after the `stopped` line, a `--play` that stopped early or ignored Ctrl+C, a token file left behind.
  - `tests/common`: `TemporaryFolder.h` (non-ASCII temp folders) and `FakeHocket.h` (a bus, handler and API server on a thread of their own, plus a token file: a running Hocket, as far as a client can tell).
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.
- **Threads**, as the code review of 2026-10-31 found them:
  - No Hocket code runs on the audio thread. `EditSync` changes the Edit on the message thread; Tracktion builds the new graph there and destroys the retired one there too, after its pool threads let go (`LockFreeMultiThreadedNodePlayer::postNewGraph`).
  - IXWebSocket's threads only post to the message thread; the bus, handler, the server's connection map and `EditSync` are only used there; `Client`'s shared state is behind its mutex; the Ctrl+C flag is atomic.

## Set up on Lee's laptop
`hocket-mcp.exe` is installed in `%LOCALAPPDATA%\Hocket\bin` and registered in Claude Code as `hocket` at user scope (API build `c4f35c9e10c86405`). Every Claude Code session gets the tools; they say "Hocket isn't running" until `--play` runs. After a change to the tools or the API, run CLAUDE.md's install command again and restart Claude Code sessions.

**To do when Lee sets up Claude Desktop** (not started; don't drop this):
1. Register the installed `hocket-mcp.exe` in Claude Desktop's config. It should be `%APPDATA%\Claude\claude_desktop_config.json`, under `"mcpServers": { "hocket": { "command": "<path to hocket-mcp.exe>" } }`; check against Claude Desktop's own docs. Then restart Claude Desktop.
2. Find the `clientInfo` name it sends. hocket-mcp logs `client '<name>' <version>, MCP <version>` to stderr on `initialize`, and Claude Desktop keeps servers' stderr in its MCP logs (expected in `%APPDATA%\Claude\logs\`, e.g. `mcp-server-hocket.log`). `--play`'s `API: ... connected` line shows the name Hocket ends up using.
3. Map that name to "Claude" in `mcp::authorName` (`src/mcp/Bridge.cpp`). Add a check to "names and labels are made to fit hello" in `tests/mcp/BridgeTests.cpp`. Update SPEC §6.3's `client` line and the "Other MCP clients" section of `docs/connecting-claude-code.md`. Rebuild and reinstall.
4. Check the label too. Claude Desktop isn't started in a project folder, so the working folder's name may mean nothing (an app or system folder). If so, pass `--label` in its config, or pick a better default for clients that don't start the bridge in a project folder.

## Test hardware
- **Laptop** (main development machine): Magni Mesh DAC, used at the desk. It has no ASIO driver of its own. ASIO4ALL put a buzz over the bass (Loose ends); FL Studio ASIO works on the Magni, so the ASIO checks use it here. Away from the desk, headphones go on the Realtek; FL Studio ASIO wouldn't start there on 2026-11-12, but Realtek ASIO works (Loose ends). Device names as `--list-devices` prints them: `FL Studio ASIO`, `ASIO4ALL v2` and `Realtek ASIO` under ASIO; the Magni under Windows Audio is `Speakers (Magni Mesh Dac)`. None of these drivers counts xruns (SPEC §9).
- **Desktop**: Focusrite Scarlett 2i2 with Focusrite's native ASIO driver and mic inputs, the test interface for audio recording (Milestone 7). Lee's Yamaha piano connects to it for MIDI input and recording (Milestone 3).
  - **It needs the toolchain by Milestone 3**: Visual Studio 2026, Git, a clone with its submodules, and a build that passes its tests (CLAUDE.md), then `hocket-mcp` installed and registered there. Check its free memory and disk first (Gotchas: build time).

## Loose ends
- **ASIO4ALL v2 on the Magni buzzes (2026-11-04).** Sharp, buzzy "NES helicopter" sounds came with the bass line. Windows Audio and FL Studio ASIO, on the same DAC, sounded fine. The cause isn't known yet. **The check, when Lee is at the Magni:** turn the Magni's knob well down first (ASIO4ALL most likely bypasses the Windows volume, and on 2026-11-06 it was very loud at Windows volume 2), then `--play examples\m0-demo --driver ASIO --device "ASIO4ALL v2" --sample-rate 48000` for a few seconds, listening for the buzz. Afterwards, put the device back to FL Studio ASIO (next loose end).
  - Without `--sample-rate`, Hocket opens the device with whatever the driver or Tracktion's saved settings give, and prints both in its `Playing on` line. With no saved rate, JUCE takes the driver's current rate.
  - The log points at the sample rate, not the buffer. Both ASIO drivers ran at 44100 Hz with 512 samples, but Windows runs the Magni at 48000 Hz (Windows Audio offers it at 48000 Hz only). FL Studio ASIO goes through Windows' audio engine, which converts the rate. ASIO4ALL drives the device directly, by kernel streaming, so it needs the DAC itself at 44.1 kHz while Windows holds it at 48 kHz.
  - The test: `--sample-rate 48000` with ASIO4ALL. Other ways: set the Magni to 44100 Hz in Windows' Sound settings (the device's Properties, then Advanced) and try ASIO4ALL again, or turn on "Always Resample 44.1kHz <-> 48kHz" in ASIO4ALL's advanced panel.
  - Plenty of people only have ASIO4ALL, and if Hocket opens it with settings that don't work, picking better ones is Hocket's job.
- **FL Studio ASIO wouldn't start with headphones on the Realtek (2026-11-12).** "Device didn't start correctly", with or without `--sample-rate`. Nothing else was using audio, FL Studio wasn't running, and Realtek ASIO opened fine straight after. Its output setting (`HKCU\Software\Image-Line\ASIO`, `outputEndPoint`) is blank, which seems to mean the Windows default output. Without `--sample-rate`, Hocket opens it the same way as in Milestone 0, when it worked on the Magni, so the setup is more likely than step 1's code.
  - **The check, at the Magni:** `--play <a silent project> --driver ASIO --device "FL Studio ASIO" --sample-rate 44100`. If it opens, the problem was the headphones setup; if not, look at Hocket. Either way this puts the saved device back, which is Realtek ASIO at 44100 Hz until then.
- **No text selection while `--play` runs (2026-11-04).** Not being fixed: the GUI replaces `--play` for everyday use (decided 2026-11-06). The cause is the review's fix #10, which turns QuickEdit off, so that a selection can't pause console output and with it the message thread. Shift+drag should still select in Windows Terminal. If it's ever needed: write console output from a thread of its own, through a queue, and leave QuickEdit as the user had it.
- **Clip starts in `project.json` have no upper bound** (Milestone 1's step 2). A clip is at most 999 bars long, but one can start at bar 100000, which makes as long a loop. Only a hand-edited `project.json` can do it today.
- **One unexplained stress-test failure:** on 2026-10-30, a Debug run of the two stress tests failed once, right after a full Debug `ctest`. My filter hid the error, so which check failed isn't known. It didn't happen again in 23 Debug runs since: 11 of the stress tests alone, and 12 of the whole engine suite. If it shows up again, keep the whole output.
- **Tracktion stops playback when it rebuilds its device lists** (Milestone 3 decides what Hocket does). Applying a new MIDI device list (or an audio device change) releases every playback context, which stops the playhead, and the transport then stops itself. `--play` works around the one that always happens: Tracktion's first MIDI scan otherwise lands 4 s after startup (see Gotchas). A MIDI device plugged in during playback would still stop it, and Lee plugging the piano in mid-session is realistic.
  - It happened over ASIO on 2026-11-04. During the first of Lee's ASIO4ALL runs, Lee changed the output device in ASIO4ALL's panel, and Ctrl+C later found playback "already stopped". That fits: a driver asks for a reset when its settings change (`kAsioResetRequest`), and JUCE then restarts the device. Reproduced on purpose on 2026-11-12: changing any setting in ASIO4ALL's panel stops playback.
  - `--play` now says when playback stops by itself, and what changed on the device if anything did. It doesn't restart it: an API client's transport play does.
- **API:**
  - `history` has no author filter yet (SPEC §6.4; Milestone 1's step 10).
  - The person's display name is `--user` only; it becomes a saved setting with the GUI's settings (Milestone 2).
  - When the server shuts down, open API transactions aren't interrupted: the process is exiting, and history isn't saved.
  - A client whose last connection closed with an undelivered note keeps a small entry in the handler until it reconnects. It's never pruned.
  - No limit on message size: IXWebSocket has none. Localhost only, so it's left.
  - Replies are sent from the message thread, and IXWebSocket's server sends block until written. With the 2-second send timeout, a client that stops reading can hold up the message thread for at most that long before it's disconnected. The audio thread is never affected.
- **MCP bridge:**
  - Only Claude Code's `claude-code` maps to "Claude". Other clients appear under the `clientInfo` name they send, unless their config passes `--name`. Claude Desktop's name gets mapped once it's checked (Set up on Lee's laptop); add others as clients are tried.
  - It handles one request at a time: a call waits for Hocket's answer before the next message is read. That's fine while the DAW handles requests one at a time anyway. `notifications/cancelled` is ignored, and a `ping` sent during a call is answered after it.
  - Replies time out after 30 s, and `hello` uses the same limit. `render_section` and the analysis tools (Milestone 8) need longer, or progress notifications.
  - Supported MCP versions are 2024-11-05 to 2025-11-25; a client asking for a newer one is offered 2025-11-25. Claude Code 2.1.314 asks for 2025-11-25, but its MCP log calls that `"protocolEra": "legacy"`, so a newer era exists. Check what it changes before a future Claude Code stops accepting the old one.
  - A refused connection to a port on 127.0.0.1 takes about 2 s on Windows. It only happens when the token file names a running Hocket that isn't listening.
- **Command bus:** continuous-gesture coalescing (SPEC §3.1) comes with volume and pan, the first continuous settings (Milestone 1's step 4). SPEC §6.4 has no MCP `redo`; if one is added, it gets the same author check as `undo`, applied to whoever did the undo.
- **Sync coverage:** `EditSync` handles `ClipContentChanged` only, and asserts that clips are never added or removed (Milestone 1's steps 5 and 6). Each new command that changes the project needs its sync code and a render or live test, and each new kind of change to the Edit a stress test (SPEC §3.3).
- **Tracktion's undo** can't be fully off: JUCE keeps at least one step even at 0 levels, and some Tracktion setters (tempo, clip position) record into it. `EditSync` clears it after every sync; nothing ever undoes in the Edit.
- **Render speed.** An offline render of a few seconds takes about 1.6 s (Release), and each live test about 0.5 s: most of the engine tests' time outside the stress tests. The cost looks like 4OSC building its wavetables each time it initialises ("Generating waves" in the Debug log). Hocket's synth replaces 4OSC in Milestone 4, before `render_section` and the analysis tools need speed (Milestone 8).
- **Debug engine tests:** no JUCE assertions fire and JUCE's leak detector is silent, but the MSVC CRT reports two to four 16-byte blocks at exit, even when no test case runs. Not chased.
- **`--watch`** compares each file's modification time and size. A same-size rewrite within the file system's timestamp resolution could be missed, which is unlikely in practice.
- **Saving to disk:** `project::write()` produces every file in memory; writing them to the folder isn't built (Milestone 1's step 9).
- **Tracktion pin:** upstream `develop` at 3.5.0 is untagged, and the submodule is Hocket's fork with one fix on top (DEPENDENCIES.md). Move to the 3.5.0 tag when it exists, following DEPENDENCIES.md's "Updating Tracktion". It covers the fork's commit and Hocket's two workarounds (the first MIDI scan, and `setUsesProxy (false)`), each kept until upstream has its own fix, and moving the JUCE pin to match.
- **Known upstream issues (not reported, by Lee's decision, 2026-11-06).** Tracktion takes bug reports only on the JUCE forum, whose community guidelines don't allow content generated by AI ([policy](https://forum.juce.com/t/new-policy-no-content-generated-by-artificial-intelligence/66362)). Hocket works around or fixes each one itself instead.
  - The three Tracktion reports Claude drafted (the first MIDI scan, the dropped block, and `MidiNode`) stay unposted in Lee's `Documents\Hocket\upstream`, untouched, in case Lee decides otherwise. Anything posted would have to be in Lee's own words, per that policy.
  - Each time Tracktion is updated, DEPENDENCIES.md's "Updating Tracktion" checks whether the first three Tracktion items below are still needed.
  - **Tracktion, the dropped block:** `LockFreeMultiThreadedNodePlayer::prepareToPlay` calls `createThreads()`, which takes `processMutex`, for every graph rebuild, so the audio thread's `try_lock` sometimes fails and a whole block plays as silence (a 5.8 ms click, about once per 1,700 rebuilds). Fixed in Hocket's fork, kept long-term (DEPENDENCIES.md). How it was found: Gotchas, "Debugging Tracktion's playback".
  - **Tracktion, the first MIDI scan:** `DeviceManager::initialise()` calls `rescanMidiDeviceList()` (a 5 ms timer) and then `restartMidiCheckTimer()`, which replaces it with the 4 s rescan timer. So the first MIDI scan lands 4 s after startup and stops anything already playing. Workaround: `--play` runs the scan itself before playing (`src/app/Main.cpp`; Gotchas).
  - **Tracktion, `MidiNode`'s notes:** `MidiNode` (`tracktion_MidiNode.cpp`), which plays MIDI clips that use a proxy sequence (the default), keeps no note state across graph swaps.
    - `prepareToPlay` only checks whether the old graph had the node (54-60).
    - The new node then sends the new sequence's note-offs unchecked (171-197), and its loop-end note-offs come from that sequence too (201-205).
    - So a rewrite that changes a sounding note leaves the old one on for good. `LoopingMidiNode` gets this right.
    - Workaround: `EditSync` calls `setUsesProxy (false)` on each clip, so clips play through `LoopingMidiNode`. Repro: `hocket_engine_tests --test-case="stress: rewrites that change*"` with that call taken out.
  - **Tracktion, 4OSC allocates on the audio thread:** it builds a `juce::MidiBuffer` for every 32-sample sub-block (`FourOscPlugin::applyToBuffer`, `tracktion_FourOscPlugin.cpp:1484`), and `addEvent` allocates whenever a sub-block has MIDI. Noticed while chasing the stuck notes; no glitch traced to it. Hocket's synth replaces it in Milestone 4.
  - **Tracktion, `SamplerPlugin` locks and allocates on the audio thread:** `applyToBuffer` takes `lock`, a `CriticalSection` the message thread also takes (`tracktion_SamplerPlugin.cpp:297`), and creates a `new SampledNote` for every note-on (`:336`), deleted on the audio thread when it ends. Found while planning Milestone 1 (2026-11-06), so Hocket's drum sampler is its own from the start (SPEC §8.1).
  - **Tracktion, the version string:** `Engine::getVersion()` is hardcoded to "v3.1.0"; we read `VERSION.md` instead.
  - **IXWebSocket:** its server sets `SO_REUSEADDR` unconditionally (`IXSocketServer.cpp:135`), so on Windows a second server can bind a port another is listening on. `listen()` is non-virtual and the socket private, so `SO_EXCLUSIVEADDRUSE` can't be set without forking. Also, its server sends block with no timeout by default (`kSendTimeoutSeconds = -1`); `Server.cpp` sets 2 s.

## Gotchas picked up so far
- **Line endings.** This machine has `core.autocrlf=true`. `.gitattributes` keeps `*.notes`, `*.auto` and `project.json` LF; add any new byte-compared fixture type there. In the Bash tool, `grep $'\r'` reports every line; count CRs with `tr -cd '\r' < file | wc -c`.
- **Don't name a helper `quoted`.** Argument-dependent lookup finds `std::quoted` instead (error C2676). The code uses `singleQuoted` / `doubleQuoted`.
- **nlohmann/json** is built with implicit conversions off: write `get<T>()`.
- **Property tests against the bus** must canonicalize their reference copy with `*notes::parse (text).clip`, because the bus stores the parsed clip (spellings filled in), not the text. The parsed clip isn't canonical, though: notes and comments stay where the text had them. To ask whether two clips are the same, compare `notes::write` of each, as the bus does for "changes nothing".
- **doctest has no `GENERATE`** (that's Catch2): loop over a table, with `INFO` for the case.
- **Windows PowerShell 5.1 reads a `.ps1` without a byte order mark as ANSI**, so keep scripts ASCII (`tests/acceptance` builds its non-ASCII folder name from `[char]` codes). And an expression can't continue onto a line that starts with `+`: build the string first.
- **Build time.** JUCE and Tracktion compile once into `hocket_juce`. A clean Release build takes 1 to 2.5 minutes here (2 min 21 s in a fresh clone on 2026-10-30). A fresh clone's time is mostly the submodule download, which varies a lot: 21 minutes on 2026-10-30, but the whole `-FreshClone` step took 4.7 min on 2026-11-06. The download is about 1 GB. The parallel build needs several GB of free memory: a run on 2026-11-06 was killed while ComfyUI held a model in memory (a `python` process using 9.6 GB).
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
- **Creating a JUCE audio device loads its driver.** `AudioIODeviceType::createDevice` returns a device that isn't open, but for ASIO the constructor loads and initialises the driver (`ASIOAudioIODevice` calls `openDevice()`). So asking a device for its rates and sizes isn't free: `--list-devices` only does it for the devices named, and the GUI's audio settings (Milestone 2) should only do it for the device being set up. WASAPI devices only query formats.
- **The device manager takes the nearest rate it can get.** Asked for a rate or buffer size the device doesn't offer, `AudioDeviceManager::setAudioDeviceSetup` opens at the nearest one it does offer without an error, and Tracktion then saves that for the next run. `openDevice` checks against the open device's lists first.
- **Custom Tracktion devices:** Tracktion lets a program register plugin types of its own with `PluginManager::createBuiltInType<T>()` (`tracktion_PluginManager.h:75`; the DemoRunner's `DistortionEffectDemo.h` is an example). Hocket's devices use it (Milestone 1's step 7 onwards).
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
- **Commit messages from the PowerShell tool:** Windows PowerShell 5.1 strips the double quotes inside a native program's argument and splits it there, so `git commit -m` with a quoted word in the message fails ("pathspec ... did not match"). Write the message to a file in the scratchpad and use `git commit -F <file>`.
- **`gh` (the GitHub CLI) is set up**, since 2026-11-06: version 2.103, logged in as `pastaplsok`, with the `repo`, `read:org`, `gist` and `workflow` scopes.
  - Ask Lee before creating anything on GitHub with it (forks, pull requests, issues), and never use it for upstream reports (Known upstream issues).
  - `git push` still goes through Git Credential Manager (the system gitconfig's `credential.helper manager`).
  - A Claude Code session started before `gh` was installed won't find it on PATH; use `C:\Program Files\GitHub CLI\gh.exe`.
- **Deleting and discarding is Lee's job.** Claude Code's auto-mode check has blocked Claude from deleting temp folders and from `git checkout --` on a changed file. Give Lee the command to run in their own window instead (e.g. `Get-ChildItem $env:TEMP -Directory -Filter 'hocket-*' | Remove-Item -Recurse -Force` for the acceptance runs' leftovers).
- **Long background runs can be killed under memory pressure.** Claude Code stops background commands when the system runs critically low on memory, even if they aren't the cause. Check free memory before a fresh-clone run or a long build, and ask Lee about memory-heavy apps first.
- **Debugging Tracktion's playback**, as with the stuck notes and the dropped block (2026-11-04 to 06):
  - Record what reaches the synth: `MidiRecorder` (`tests/engine/StressTests.cpp`) is a test plugin put ahead of 4OSC. It shows the MIDI the synth really gets, in order. Logging each block's `context.editTime` there shows skipped blocks too: a jump of exactly one block means the plugins never ran for it.
  - Make it happen often: a "hammer", a temporary test on `StressTests.cpp`'s real-time harness doing 1,000 comment-only rewrites at 100/s to a held note, turned a 1-in-1,700 bug into one every few runs. Compare 20 or more runs before and after, then take the test out again (it's slow).
  - Look inside Tracktion: add temporary logging to the submodule, appending to a static, preallocated array with an atomic index (nothing allocates on the audio thread), and read it from the test through `extern` declarations. Take it out by hand afterwards and check `git -C external/tracktion_engine status` is clean, since `git checkout` may be blocked.
  - Read the node that actually plays: a MIDI clip plays through `MidiNode` or `LoopingMidiNode` depending on `setUsesProxy` (above).
