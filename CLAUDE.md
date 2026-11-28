# CLAUDE.md

**Hocket** — an AI-native DAW where a human and an LLM co-produce music. Windows-first, C++ / JUCE / Tracktion Engine, open source.

**SPEC.md is the source of truth** for what we're building and why. Read it before starting work. If a decision changes during implementation, update SPEC.md in the same change — never let code and spec silently disagree.

## Current phase
v1 is planned (SPEC.md §10.1, agreed with Lee on 2026-11-06): nine milestones, after Milestone 0's headless proof. Milestone 1, "a song, not a loop", is next and still headless; its steps are in PROGRESS.md, and the records of those done in `docs/history/milestone-1.md`. Plan each later milestone's steps with Lee when it starts.

Read PROGRESS.md at the start of a session; update it at the end of each step (step status, the next step, loose ends).

## Build & run
- Toolchain: Visual Studio 2026 (v18, MSVC 14.51), CMake ≥ 3.25 (VS ships 4.3), Git. `cmake` isn't on PATH by default; use the "Developer PowerShell for VS" or add `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
- Dependencies: git submodules under `external/`, pinned to exact commits. Tracktion Engine comes from Hocket's public fork, upstream plus one fix, kept long-term. Versions, licenses, the fork, and the steps for updating Tracktion (including checking whether Hocket's Tracktion workarounds are still needed): `DEPENDENCIES.md`.

```
git submodule update --init          # not --recursive (see DEPENDENCIES.md)
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
.\build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --list-devices
.\build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --check examples\m0-demo
.\build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --new "$env:USERPROFILE\Music\my-song"
.\build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --play examples\m0-demo --driver ASIO --device "FL Studio ASIO" --watch --user Lee
cmake --install build\vs2026 --config Release --component mcp --prefix "$env:LOCALAPPDATA\Hocket"
.\build\vs2026\tools\stress\Release\hocket-stress.exe
powershell -ExecutionPolicy Bypass -File tools\acceptance\Run-Acceptance.ps1
powershell -ExecutionPolicy Bypass -File tools\acceptance\Check-CtrlCWhileLoading.ps1 -Driver ASIO -Device "FL Studio ASIO"
python tools\kits\make-electronic-kit.py          # --check: does the kit on disk match?
```

- Run these from the repository's folder. PowerShell needs the `.\` to run a program by a relative path; without it, `build\...` is taken for a module name.
- On Lee's laptop, ASIO means `FL Studio ASIO`; ASIO4ALL buzzes there at 512 samples, but is clean at `--buffer-size 1024` (PROGRESS.md, Loose ends).
- Silent runs may use the laptop's built-in audio without asking: `--driver "Windows Audio" --device "AI Noise-cancelling Output (ASUS Utility)"` (the Realtek speakers won't open directly; PROGRESS.md, Test hardware). A test that needs the Magni: tell Lee first.

- `--check <folder>` validates a project folder and prints every error as `file:line:column: message`. Run it after editing project files by hand.
- `--new <folder>` makes an empty project (120 BPM, 4/4, no tracks) in a new or empty folder, making the folder if need be, and refuses a folder with anything in it (SPEC §4.5).
- `--play <folder>` loops the project until Ctrl+C, or until its console window is closed, and serves the API (below). Its options: `--driver`, `--device`, `--sample-rate` and `--buffer-size` (without them, the last device, rate and size, which Tracktion saves in `%APPDATA%\Hocket\Settings.xml`), `--watch` (files saved outside Hocket taken in as your commands), `--user <name>` (your name in the history), `--master-volume <dB>`, and `--recover` or `--discard-recovery` for a recovery copy a session left. In its console, `S` saves, and `O` saves over files changed outside Hocket after `S` was refused. What it does in full (kits, the master level, the recovery copy, interrupted saves, the console keys, the exit report): `docs/headless-play.md`. Read it before changing `--play`, or driving it from a tool.
- `--list-devices --driver <type>` or `--device <name>` also lists the sample rates and buffer sizes of the devices named. That loads their drivers without starting them. An ASIO driver loaded this way can disturb another program using the device, so ask Lee before doing it for ASIO.
- While `--play` runs, it serves the WebSocket API (SPEC §6.3) on 127.0.0.1, port 46253 or the next free one; the port and token are in `%LOCALAPPDATA%\Hocket\api.json`, and API activity is printed with an `API:` prefix. A second `--play` refuses to start while one is running or starting: each holds `%LOCALAPPDATA%\Hocket\api.lock` until it exits.
- `hocket-mcp` is the MCP server Claude Code starts to reach that API (SPEC §6.3). Claude Code runs the copy that `cmake --install ... --component mcp` puts in `%LOCALAPPDATA%\Hocket\bin`, never the one in `build\`. Install again after changing the tools or the API, or every call fails with "different builds". Registering it, and the rest: `docs/connecting-claude-code.md`.
- `hocket-stress` rewrites a clip in the running `--play` 200 times at 20 a second, then puts it back (Milestone 0's stress test; `--help` for the options). By default only a comment changes, so the music shouldn't: listen for clicks or gaps.
- `tools\acceptance\Run-Acceptance.ps1` runs Milestone 0's acceptance checks that don't need ears, silently, against a real `--play` on a copy of the demo with an empty clip. `-Driver`/`-Device` pick the device, and `-FreshClone` also clones and builds from scratch: about 1 GB to download, and several GB of free memory for the build, so check with Lee first. It's the way to run `--play` from a tool without sound, and to stop it cleanly with Ctrl+C. Its silent project also has a drums track playing the bundled electronic kit, so every run loads a kit. What it makes of a run's end is in `AcceptanceChecks.ps1`, which CTest's `acceptance_checks` tests.
- `tools\acceptance\Check-CtrlCWhileLoading.ps1` is step 8's check that a Ctrl+C while a kit is loading stops `--play` cleanly: silent, on the audition project with its clips emptied, at 48 kHz so the kit takes a moment to convert. It opens the device, so it's Lee's to run.
- `tools\kits\make-electronic-kit.py` (Python 3 and numpy) synthesizes the bundled electronic kit into `assets\kits\electronic` from named parameters for each piece: a change asked for by ear is a parameter and a re-run. `examples\electronic-kit` is its audition project.
- Tests never open a real device, touch that settings file, or write the real token file or recovery folder. The acceptance scripts play projects in new folders each run, so a recovery copy a crashed run left can't stop their `--play`; `Run-Acceptance.ps1` clears away its own earlier runs' copies, and checks that one is written and removed, and that after `--play` is killed outright with a change unsaved, plain `--play` stops and asks, `--recover` plays the change as one undoable entry, and `--discard-recovery` files it as a kept save. The API tests use ports from 46300, the bridge tests from 46320, the stress client's from 46340. The engine tests take about 3 minutes, most of it stress tests playing in real time. They ask Windows for 1 ms timers, or Tracktion's `play()` can take 2.5 s (PROGRESS.md, Loose ends).
- `--play` makes sound. Don't run it on someone's machine without asking; a project with an empty clip plays silence, which is enough to check that the device opens.

- If `git submodule update` reports `Filename too long`, the clone path is too deep for a few of JUCE's files (under `extras/`, which we don't build). Run `git config --global core.longpaths true`, then `git -C external/JUCE reset --hard`; or clone to a shorter path.
- JUCE and Tracktion compile once into the `hocket_juce` static library; Hocket targets link it. Our own targets get warnings via `hocket_enable_warnings()` (`cmake/HocketWarnings.cmake`); never apply those to third-party code.
- Programs that read or write project folders get `hocket_long_paths()` (`cmake/HocketLongPaths.cmake`): a manifest that lifts Windows' 260-character limit where its long-path setting is on (SPEC §9).

## Non-negotiable rules
- **Never block or allocate on the audio thread.** No locks, no file/network I/O, no heap allocation in real-time callbacks. All state changes go through the command bus and are handed to the engine safely (SPEC §3.2).
  - **The one exception: JUCE's audio callback lock** (accepted by Lee, 2026-11-16). The audio thread holds it for each block, so while the message thread holds it, no block can start, and the audio thread waits. Tracktion takes it itself to swap in a new tempo map. `EditSync` takes it (`audioCallbackLock()`) only where the playhead and what it reads must change together between two blocks: a new tempo with the playhead's place and loop (`syncTempo`), and the playhead's place with a new loop, or with loop on or off (`updateLoop`).
  - **While it's held, only:** read the playhead's place; set its place and loop; arithmetic on positions; setting the tempo through Tracktion (`TempoSetting::set`, then `TempoSequence::updateTempoData()`), which changes a ValueTree property and builds the new tempo map, with a few small allocations on the message thread, then swaps it in; and reading the clock, to time the hold (`EditSync::AudioLock`, `longestAudioLockHold()`). Take it only through `AudioLock`.
  - **Never while it's held:** file or network I/O; waiting on another thread or taking another lock; sending change notices or calling into the bus; making, changing or deleting tracks, clips or plugins, or anything else that rebuilds Tracktion's graph; logging; anything that can take more than microseconds. Work anything out before taking it.
  - A new use needs Lee's agreement, and a stress test that measures how long it's held, reported next to the slowest block.
- **Every state change is a command.** GUI, scripts, and the MCP server use the same command bus. No client gets a back door.
- **Canonical file output.** The serializer always writes the same project the same way. Any change to the format or serializer needs round-trip tests (load → save → byte-identical).
- **Stable IDs, never list indices**, in the project format and the API.
- **Binary data never goes inline** in text project files (audio, plugin state).
- **In a project folder, storage code never deletes a file whose text exists nowhere else**: not on disk, and not in the running session whose save wrote it (a failed save's own temporary files hold the session's text, so they may go). Anything else is set aside in the project's `unused/`, keeping its path (Lee, 2026-11-24). Where code deletes, a comment says where else the text is.
  - **The rule covers project folders, so the recovery copy's half-written folder is the exception** (Lee, 2026-11-24). A `writing <n>` folder in `%LOCALAPPDATA%\Hocket\Recovery` that a crash cut short is cleared by the next write. It was never whole, so it can't be recovered as a project. The copy before it is whole, and holds every change up to when it was written. All the half-written one may add is part of the changes made since: seconds' worth, 30 s at most. That is the recovery copy's window anyway, since a crash just before the write began would have lost them too. And there's no `unused/` outside a project to set it aside in.
- **Never implement Claude subscription login** or use subscription tokens in the app. AI clients connect via the MCP server; any built-in AI feature uses a user-supplied API key or a local model (SPEC §6.6).
- **License hygiene.** Hocket is AGPL-3.0. Every new dependency must be AGPLv3-compatible (MIT, BSD, Apache-2.0, GPLv3, AGPLv3 are fine; proprietary or GPLv2-only are not). Note each dependency and its license in the dependency list.

## Conventions
- C++20, follow JUCE naming style for JUCE-facing code
- Platform-specific code isolated behind interfaces (Linux/macOS ports come later)
- **Text is UTF-8; paths are `std::filesystem::path`.** Every `std::string` holds UTF-8 (we build with `/utf-8`, so narrow literals are UTF-8 too).
  - Convert between text and paths only with `hocket::pathFromUtf8()` / `hocket::utf8FromPath()` (`src/core/Paths.h`).
  - Never build a path from a `std::string` or `char*`, never call `path::string()`, and open files only through a `path`. On Windows the narrow forms go through the ANSI code page, which turns most non-ASCII names into `?`.
  - Text from the OS (command line, environment, Win32 calls) enters through the platform layer as UTF-8: call the wide (`W`) Windows APIs there and convert (see `hocket::platform::utf8Arguments()`). Never use `argv` directly.
  - In JUCE code, `juce::File` is fine; convert strings with `juce::String::fromUTF8()` and `toStdString()`.
  - Test new file-handling code with a non-ASCII folder name (see `tests/core/PathsTests.cpp`, `tests/app/CMakeLists.txt`).
- Tests for: `.notes` parser, `.auto` parser, serializer round-trips, command validation, undo/redo
- Saving's tests stop (as a crash would) and make fail every step of a save, its put-back, finishing an interrupted save, and setting leftovers aside, and check the never-delete rule and the all-or-nothing rule after each one (Lee, 2026-11-24)
- Error messages from validation are written for an LLM reader: say what's wrong, where, and what valid input looks like

## Working style
- Propose a plan before large changes; keep milestones small
- When something in the spec turns out to be wrong or awkward in practice, say so and propose the fix rather than working around it
- Branches: milestone steps commit straight to `main`. A larger batch of fixes (a code review's, say) goes on a branch, pushed, with a PR to merge it.
- Commit after each part of a step's build order, updating PROGRESS.md as each part lands (Lee, 2026-11-12).
- A batch of fixes starts with each finding's test, failing (`doctest::should_fail`, or skipped if it would end the run), committed first; each fix takes its own mark off (Lee, 2026-11-24 and 2026-11-25).
- Small follow-ups that finish a step Lee has approved (a fix, a clearer message, a missing test) can be committed and pushed without asking. Anything that changes the file format, a spec decision, or behaviour outside the step waits for Lee.

## Gotchas
The ones that bite in every session. The rest, by area (Tracktion and JUCE, tests, libraries, Windows and the tools, MCP), are in `docs/gotchas.md`: read the part for an area before working in it.
- **Line endings:** `core.autocrlf=true`, so checked-out sources are CRLF and files the Write tool makes are LF; git's "LF will be replaced by CRLF" is harmless. A script that edits a file keeps its line endings (Python: `newline=''`).
- **Building from the tools:** `cmake` isn't on the PowerShell tool's PATH. Prepend `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` to `$env:PATH` in the same command, and filter a build with `Select-String -Pattern "error|warning C"`: no output is a clean build.
- **The PowerShell tool's working folder follows a Bash `cd`:** start such commands with `Set-Location D:\Projects\Hocket`, or use absolute paths.
- **Bash heredocs break on quotes and backslashes:** make code edits with the Edit tool; for a scripted edit, write the script to the scratchpad and run it from there.
- **Commit messages:** write them to a file in the scratchpad and `git commit -F` it; Windows PowerShell 5.1 splits a quoted argument.
- **Don't build while an acceptance run is running:** the build replaces the programs it runs.
- **Deleting and discarding is Lee's job:** auto mode blocks deleting temp folders and `git checkout --`, so give Lee the command to run.
- **Long background runs can be killed under memory pressure:** check free memory before a fresh-clone run or a long build.
- **`gh` is set up** (as `pastaplsok`): beyond a batch of fixes' PR, ask Lee before creating anything on GitHub with it, and never use it for upstream reports.
