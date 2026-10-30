# Progress

Status board for the current milestone. Read it at the start of a session; update it at the end of each step. Keep it current, not historical — git log is the history.

## Milestone 0 — headless proof (SPEC §10.0)

| # | Step | Status |
|---|---|---|
| 1 | Build skeleton: pinned submodules, CMake + VS 2026 presets, `hocket-headless --list-devices` | ✅ Done |
| 2 | `.notes` parser + canonical writer (M0 subset), round-trip / error / property / fuzz tests | ✅ Done |
| 3 | `project.json` loader + canonical writer, project model, `hocket-headless --check`, tests | ✅ Done |
| 4 | Command bus: `write_clip`, undo/redo with the AI author check, history, author tags, transactions; tests | ✅ Done |
| 5 | Engine sync + playback: Edit built from the model, 4OSC, looping over ASIO; transport commands; offline-render test | ⏭ Next |
| 6 | WebSocket API on 127.0.0.1 + token file; interrupted-transaction handling | ⬜ |
| 7 | `hocket-mcp` stdio bridge + doc on connecting Claude Code | ⬜ |
| 8 | Stress test, acceptance run, SPEC updates | ⬜ |

**Milestone 0 is done when:**
- a fresh clone builds with the commands in CLAUDE.md;
- the demo project loops over ASIO;
- from Claude Code, `write_clip` is heard on the next loop with no glitch, `undo` restores the old clip, and `history` shows who made each change;
- 200 rewrites at 20/sec during playback cause zero xruns (where the driver reports them) and no audible dropouts;
- all tests pass.

## What exists
- `src/core` (`hocket_core`, no JUCE): `notes/` (`.notes` model, parser, canonical writer), `project/` (`project.json` model, reader, writer), `commands/CommandBus` (the only way to change a `project::Project`), `Paths` (UTF-8 ⇄ path).
- `src/app` (`hocket-headless`): `--list-devices`, `--check <folder>`, `--version`. `Console_windows.cpp` is the platform layer (UTF-8 console, wide command line).
- `tests/core` (doctest, `hocket_core_tests`): 61 cases. `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder.
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.

## Next: step 5 — engine sync and playback
- New `hocket_engine` library (links `hocket_core` and `hocket_juce`): builds a Tracktion `Edit` from `project::Project` — tempo, 4/4, one track per Hocket track with a 4OSC, one MIDI clip per clip — with Tracktion's own undo off (SPEC §3.3).
- Subscribes to the bus (`CommandBus::addListener`, `ClipContentChanged`) and rebuilds only that clip's MIDI, on the message thread. The listener fires after undo, redo and rollback too.
- `hocket-headless --play <folder>`: opens ASIO (or WASAPI), loops the whole arrangement, Ctrl+C to quit.
- Transport commands (play, stop) go through the bus without entering the history (SPEC §3.1); they need the engine, so they land here.
- Offline-render test (no audio device): the render isn't silent, notes start where `.notes` says, and a `write_clip` changes the render.
- Settle the Tracktion settings file (see below).

### Step 5 notes and gotchas
Pointers below are into `external/tracktion_engine/modules/tracktion_engine/`. They are what the source says; the playback behaviour still needs verifying.
- **Startup.** `src/app/Main.cpp` already makes a `tracktion::Engine` with a custom `EngineBehaviour` (`autoInitialiseDeviceManager()` returns false, so `--list-devices` opens nothing), inside `juce::ScopedJuceInitialiser_GUI` (Tracktion needs JUCE's message manager even headless). `--play` needs the device manager initialised and a running message loop (`juce::MessageManager::getInstance()->runDispatchLoop()`). Ctrl+C arrives on another thread: install the handler in `Console_windows.cpp` (`SetConsoleCtrlHandler`, behind `Console.h`) and have it stop the loop, then shut down on the message thread.
- **Undo off.** `Edit::Options` (`model/edit/tracktion_Edit.h:107`) has `numUndoLevelsToStore`; set it to 0, and pass `nullptr` for every `juce::UndoManager*` argument, e.g. `MidiList::addNote (pitch, BeatPosition, BeatDuration, velocity, colourIndex, UndoManager*)` (`midi/tracktion_MidiList.h:84`). `Options::numAudioTracks` defaults to 1; make it match our tracks. MIDI clips live on Tracktion `AudioTrack`s.
- **4OSC** is `tracktion::FourOscPlugin`, type name `"4osc"` (`plugins/effects/tracktion_FourOscPlugin.cpp:1119`). Our project calls it `tracktion:4osc` (`project::DeviceType::fourOsc`). Check that its default patch makes sound; the render test will catch silence.
- **Time.** Hocket counts ticks, 960 per beat (`notes::ticksPerBeat`, `src/core/notes/Notes.h:16`), 0-based: `project::Clip::start` is ticks from the song start, note starts are ticks from the clip start. Tracktion wants `BeatPosition` / `BeatDuration` in beats, so divide by 960.0. Clip length comes from `@length` (`notes::Clip::lengthInBars`).
- **Live updates — the main risk.** Tracktion rebuilds its playback graph when clips change: `Edit::restartPlayback()` (`model/edit/tracktion_Edit.cpp:1222`) sets a flag and starts a 1 ms timer, and clip changes call it themselves. Check by ear that replacing a clip's notes while looping is glitch-free and lands on the next loop, and keep in mind step 8 does this 20 times a second. If rebuilds glitch, look at how Tracktion swaps graphs before working around it.
- **Offline render.** `utilities/tracktion_TestUtilities.h` shows the pattern: `createTestEdit`, `renderToAudioBuffer` (`Renderer::renderToFile (edit, file, false)` into a temp WAV, read back) and `renderToAudioBufferDispatchingMessageThread`. Rendering needs no device: use an `EngineBehaviour` like `--list-devices`'s.
- **Engine tests** go in a new test executable linking `hocket_engine`. It needs JUCE's message manager, so it probably needs its own doctest main (core tests use `doctest_with_main`) that holds a `ScopedJuceInitialiser_GUI`.
- **Settings file.** Tracktion's default `PropertyStorage` writes `%APPDATA%\Hocket\Settings.xml` and a `Temporary` folder (`utilities/tracktion_PropertyStorage.cpp:148`); this machine already has them from step 1. Its device manager saves the last audio device there. At least the tests should pass their own `PropertyStorage` (the `Engine` constructor takes one) so they never read or write the user's settings.
- **Transport commands.** `commands::Command` is `std::variant<WriteClip>` and `Change` is `std::variant<ClipContentChanged>`. Transport isn't project state, so it needs a path through the bus that skips history and validation against the project. Decide its shape here, and update SPEC §3.1 if it changes anything.
- **Devices on this machine** (`--list-devices`): ASIO has ASIO4ALL v2, FL Studio ASIO and Realtek ASIO. WASAPI has Speakers (Magni Mesh Dac). The Magni has no ASIO driver of its own, so for ASIO (steps 5 and 8) use ASIO4ALL routed to it. `--play` needs a way to pick the driver type and device; pick the option names here.

## Decided for step 6
- **Interrupted transactions** (SPEC §3.1). When the client that opened a transaction disconnects, the API commits it as "(interrupted)" (`wrote bridge (interrupted)`, or `wrote bridge (part 2, interrupted)` after a split), keeping its author. An empty one just closes. An idle timeout does the same for a client that's connected but silent; it counts from that client's last request and is minutes long (pick the value then).
  - Bus side: add something like `interruptTransaction (author)` next to `commitTransaction` in `CommandBus.cpp`, which already builds the "(part N)" names. The API calls it on disconnect and from a message-thread timer.
  - Tests: edits survive a disconnect, the entry is labeled and undoable, an empty transaction leaves no entry, a split one gets "(part 2, interrupted)", and the timeout fires only after inactivity.
- **Threading.** `CommandBus` isn't thread-safe and must only be used on the message thread (SPEC §3.2). IXWebSocket calls back on its own threads, so every request has to be posted to the message thread and its reply sent back from there.
- **`read_clip`** is a query, not a command; it comes with the API. The "unknown clip ID" message in `CommandBus.cpp` names `write_clip` and will need generalizing.
- **Authors per connection.** The undo check compares author strings, so the API decides each connection's author name. Not designed yet: two Claude sessions would both be "Claude" and could undo each other's work without `force`. Decide whether that's acceptable for M0.

## Loose ends
- **Command bus:** continuous-gesture coalescing (SPEC §3.1) isn't built; nothing sends continuous parameter changes yet, so add it with the first parameter command. SPEC §6.4 has no MCP `redo`; if one is added, it gets the same author check as `undo`, applied to whoever did the undo.
- **Saving to disk:** `project::write()` produces every file in memory; writing them to the folder (atomically, plus SPEC §6.1's external-change detection) isn't built. Milestone 0 doesn't need it.
- **Tracktion pin:** `develop` at 3.5.0 is untagged. Move to the 3.5.0 tag when it exists (JUCE pin follows; see DEPENDENCIES.md).
- **Upstream:** Tracktion's `Engine::getVersion()` is hardcoded to "v3.1.0"; we read `VERSION.md` instead. Worth reporting upstream.

## Gotchas picked up so far
- **Line endings.** This machine has `core.autocrlf=true`. `.gitattributes` keeps `*.notes`, `*.auto` and `project.json` LF; add any new byte-compared fixture type there. In the Bash tool, `grep $'\r'` reports every line; count CRs with `tr -cd '\r' < file | wc -c`.
- **Don't name a helper `quoted`.** Argument-dependent lookup finds `std::quoted` instead (error C2676). The code uses `singleQuoted` / `doubleQuoted`.
- **nlohmann/json** is built with implicit conversions off: write `get<T>()`.
- **Property tests against the bus** must canonicalize their reference copy with `*notes::parse (text).clip`, because the bus stores the parsed clip (sorted, spellings filled in), not the text.
- **Build time.** JUCE and Tracktion compile once into `hocket_juce`; a clean Release build takes about a minute here, a fresh clone plus build about four.
