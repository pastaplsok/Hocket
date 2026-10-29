# Progress

Status board for the current milestone. Read it at the start of a session; update it at the end of each step. Keep it current, not historical — git log is the history.

## Milestone 0 — headless proof (SPEC §10.0)

| # | Step | Status |
|---|---|---|
| 1 | Build skeleton: pinned submodules, CMake + VS 2026 presets, `hocket-headless --list-devices` | ✅ Done |
| 2 | `.notes` parser + canonical writer (M0 subset), round-trip / error / property / fuzz tests | ✅ Done |
| 3 | `project.json` loader, project model, validation tests | ⏭ Next |
| 4 | Command bus: `write_clip`, undo/redo, history, author tags, transaction grouping | ⬜ |
| 5 | Engine sync + playback: Edit built from the model, 4OSC, looping over ASIO; offline-render test | ⬜ |
| 6 | WebSocket API on 127.0.0.1 + token file | ⬜ |
| 7 | `hocket-mcp` stdio bridge + doc on connecting Claude Code | ⬜ |
| 8 | Stress test, acceptance run, SPEC updates | ⬜ |

**Milestone 0 is done when:**
- a fresh clone builds with the commands in CLAUDE.md;
- the demo project loops over ASIO;
- from Claude Code, `write_clip` is heard on the next loop with no glitch, `undo` restores the old clip, and `history` shows who made each change;
- 200 rewrites at 20/sec during playback cause zero xruns (where the driver reports them) and no audible dropouts;
- all tests pass.

## Next: step 3 — `project.json` loader
- M0 subset: format version, tempo, 4/4 time signature, MIDI tracks with stable IDs, one 4OSC device per track, clips (`id`, `file`, `start`).
- Loads each clip's `.notes` file through the parser; errors name the file (`clips/bass.notes:6:10: …`).
- Validation errors in the same LLM-readable style: where in the JSON, what's wrong, a valid example.
- Uses nlohmann/json (already pinned). Add `examples/m0-demo/`.

## Loose ends
- **Line endings:** add a `.gitattributes` forcing `eol=lf` for `*.notes`, `*.auto`, `project.json` and test fixtures before committing any such files. This machine has `core.autocrlf=true`, which would check them out as CRLF and break byte-identical comparisons.
- **Tracktion settings file:** the default `PropertyStorage` writes `%APPDATA%\Hocket\Settings.xml`. Decide in step 5 whether to supply our own.
- **Dropout test device:** the Magni DAC has no ASIO driver of its own; run steps 5 and 8 through ASIO4ALL.
- **Tracktion pin:** `develop` at 3.5.0 is untagged. Move to the 3.5.0 tag when it exists (JUCE pin follows; see DEPENDENCIES.md).
- **Upstream:** Tracktion's `Engine::getVersion()` is hardcoded to "v3.1.0"; we read `VERSION.md` instead. Worth reporting upstream.
