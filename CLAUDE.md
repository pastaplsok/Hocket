# CLAUDE.md

**Hocket** — an AI-native DAW where a human and an LLM co-produce music. Windows-first, C++ / JUCE / Tracktion Engine, open source.

**SPEC.md is the source of truth** for what we're building and why. Read it before starting work. If a decision changes during implementation, update SPEC.md in the same change — never let code and spec silently disagree.

## Current phase
v1 is planned (SPEC.md §10.1, agreed with Lee on 2026-11-06): nine milestones, after Milestone 0's headless proof. Milestone 1, "a song, not a loop", is next and still headless; its steps are in PROGRESS.md. Plan each later milestone's steps with Lee when it starts.

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
.\build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --play examples\m0-demo --driver ASIO --device "FL Studio ASIO" --watch --user Lee
cmake --install build\vs2026 --config Release --component mcp --prefix "$env:LOCALAPPDATA\Hocket"
.\build\vs2026\tools\stress\Release\hocket-stress.exe
powershell -ExecutionPolicy Bypass -File tools\acceptance\Run-Acceptance.ps1
```

- Run these from the repository's folder. PowerShell needs the `.\` to run a program by a relative path; without it, `build\...` is taken for a module name.
- On Lee's laptop, ASIO means `FL Studio ASIO`; ASIO4ALL buzzes there at 512 samples, but is clean at `--buffer-size 1024` (PROGRESS.md, Loose ends).

- `--check <folder>` validates a project folder and prints every error as `file:line:column: message`. Run it after editing project files by hand.
- `--play <folder>` loops the project until Ctrl+C, or until its console window is closed, which stops it just as cleanly. `--driver` and `--device` take names as `--list-devices` prints them, and `--sample-rate` and `--buffer-size` take values the device offers. Without them it reuses the last device, rate and size, which Tracktion saves in `%APPDATA%\Hocket\Settings.xml`. With `--watch`, each saved clip file is sent as a `write_clip`, with a warning when the save replaces a change made through the API. `--user <name>` is your name in the history (default `user`). If playback stops by itself (Tracktion does when its device lists change), it says so. Over ASIO, or Windows Audio in exclusive mode, it warns that the driver may bypass the Windows volume slider, so the device's own volume control sets the level. At Ctrl+C it prints the driver's xrun count (where the driver keeps one) and how many audio callbacks took longer than their block.
- `--list-devices --driver <type>` or `--device <name>` also lists the sample rates and buffer sizes of the devices named. That loads their drivers without starting them. An ASIO driver loaded this way can disturb another program using the device, so ask Lee before doing it for ASIO.
- While `--play` runs, it serves the WebSocket API (SPEC §6.3) on 127.0.0.1, port 46253 or the next free one; the port and token are in `%LOCALAPPDATA%\Hocket\api.json`, and API activity is printed with an `API:` prefix. A second `--play` refuses to start while one is running or starting: each holds `%LOCALAPPDATA%\Hocket\api.lock` until it exits.
- `hocket-mcp` is the MCP server Claude Code starts to reach that API (SPEC §6.3). Claude Code runs the copy that `cmake --install ... --component mcp` puts in `%LOCALAPPDATA%\Hocket\bin`, never the one in `build\`. Install again after changing the tools or the API, or every call fails with "different builds". Registering it, and the rest: `docs/connecting-claude-code.md`.
- `hocket-stress` rewrites a clip in the running `--play` 200 times at 20 a second, then puts it back (Milestone 0's stress test; `--help` for the options). By default only a comment changes, so the music shouldn't: listen for clicks or gaps.
- `tools\acceptance\Run-Acceptance.ps1` runs Milestone 0's acceptance checks that don't need ears, silently, against a real `--play` on a copy of the demo with an empty clip. `-Driver`/`-Device` pick the device, and `-FreshClone` also clones and builds from scratch: about 1 GB to download, and several GB of free memory for the build, so check with Lee first. It's the way to run `--play` from a tool without sound, and to stop it cleanly with Ctrl+C. What it makes of a run's end is in `AcceptanceChecks.ps1`, which CTest's `acceptance_checks` tests.
- Tests never open a real device, touch that settings file, or write the real token file. The API tests use ports from 46300, the bridge tests from 46320, the stress client's from 46340. The engine tests take about 2 minutes, most of it stress tests playing in real time (Tracktion's `play()` was once much slower: PROGRESS.md, Loose ends).
- `--play` makes sound. Don't run it on someone's machine without asking; a project with an empty clip plays silence, which is enough to check that the device opens.

- If `git submodule update` reports `Filename too long`, the clone path is too deep for a few of JUCE's files (under `extras/`, which we don't build). Run `git config --global core.longpaths true`, then `git -C external/JUCE reset --hard`; or clone to a shorter path.
- JUCE and Tracktion compile once into the `hocket_juce` static library; Hocket targets link it. Our own targets get warnings via `hocket_enable_warnings()` (`cmake/HocketWarnings.cmake`); never apply those to third-party code.

## Non-negotiable rules
- **Never block or allocate on the audio thread.** No locks, no file/network I/O, no heap allocation in real-time callbacks. All state changes go through the command bus and are handed to the engine safely (SPEC §3.2).
- **Every state change is a command.** GUI, scripts, and the MCP server use the same command bus. No client gets a back door.
- **Canonical file output.** The serializer always writes the same project the same way. Any change to the format or serializer needs round-trip tests (load → save → byte-identical).
- **Stable IDs, never list indices**, in the project format and the API.
- **Binary data never goes inline** in text project files (audio, plugin state).
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
- Error messages from validation are written for an LLM reader: say what's wrong, where, and what valid input looks like

## Working style
- Propose a plan before large changes; keep milestones small
- When something in the spec turns out to be wrong or awkward in practice, say so and propose the fix rather than working around it
- Branches: milestone steps commit straight to `main`. A larger batch of fixes (a code review's, say) goes on a branch, pushed, with a PR to merge it.
- Small follow-ups that finish a step Lee has approved (a fix, a clearer message, a missing test) can be committed and pushed without asking. Anything that changes the file format, a spec decision, or behaviour outside the step waits for Lee.
