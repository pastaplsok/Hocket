# CLAUDE.md

**Hocket** — an AI-native DAW where a human and an LLM co-produce music. Windows-first, C++ / JUCE / Tracktion Engine, open source.

**SPEC.md is the source of truth** for what we're building and why. Read it before starting work. If a decision changes during implementation, update SPEC.md in the same change — never let code and spec silently disagree.

## Current phase
Milestone 0 (SPEC.md §10.0): headless proof. No GUI code until Milestone 0 passes.

Read PROGRESS.md at the start of a session; update it at the end of each step (step status, the next step, loose ends).

## Build & run
- Toolchain: Visual Studio 2026 (v18, MSVC 14.51), CMake ≥ 3.25 (VS ships 4.3), Git. `cmake` isn't on PATH by default; use the "Developer PowerShell for VS" or add `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
- Dependencies: git submodules under `external/`, pinned to exact commits. Versions and licenses: `DEPENDENCIES.md`.

```
git submodule update --init          # not --recursive (see DEPENDENCIES.md)
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --list-devices
build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe --check examples\m0-demo
```

- `--check <folder>` validates a project folder and prints every error as `file:line:column: message`. Run it after editing project files by hand.

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
  - Text from the OS (command line, environment, Win32 calls) enters through the platform layer as UTF-8: call the wide (`W`) Windows APIs there and convert (see `hocket::utf8Arguments()`). Never use `argv` directly.
  - In JUCE code, `juce::File` is fine; convert strings with `juce::String::fromUTF8()` and `toStdString()`.
  - Test new file-handling code with a non-ASCII folder name (see `tests/core/PathsTests.cpp`, `tests/app/CMakeLists.txt`).
- Tests for: `.notes` parser, `.auto` parser, serializer round-trips, command validation, undo/redo
- Error messages from validation are written for an LLM reader: say what's wrong, where, and what valid input looks like

## Working style
- Propose a plan before large changes; keep milestones small
- When something in the spec turns out to be wrong or awkward in practice, say so and propose the fix rather than working around it
