# Dependencies

Hocket is AGPL-3.0. Every dependency must be AGPLv3-compatible (see CLAUDE.md). Add a row here in the same change that adds a dependency.

All source dependencies are git submodules under `external/`, each pinned to an exact commit. Clone with `git submodule update --init` (**not** `--recursive`; see the JUCE note below). On Windows, clones deeper than about 95 characters hit the 260-character path limit on a few JUCE files under `extras/` (not part of the build); `git config --global core.longpaths true` fixes it.

| Dependency | Pin | License | Used by |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | `7ff644f65d7daa1428c34b9738ebb5c449dc9172` (develop, after 9.0.4) | AGPLv3 | engine, app |
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine), from [Hocket's fork](https://github.com/pastaplsok/tracktion_engine) | `3221c531cf6a48a2f0b57d7d25cbd70df9c0f515`: upstream develop at `a5e44996f5f` (3.5.0) plus one fix (below) | GPLv3 (the fork is public) | engine |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | `v12.1.0` | BSD-3-Clause | API server and client (hocket-mcp, hocket-stress) |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` | MIT | core, API, MCP bridge, stress client |
| [doctest](https://github.com/doctest/doctest) | `v2.5.3` | MIT | tests |

**JUCE pin.** JUCE is our own top-level submodule, pinned to the same commit Tracktion Engine's `modules/juce` submodule points at. We don't use Tracktion's nested submodule because its URL is SSH-only (`git@github.com:`), which fails for anyone without GitHub SSH keys. CMake warns at configure time if the two pins drift apart; when updating Tracktion, move JUCE to Tracktion's new pin in the same change.

## Tracktion Engine: Hocket's fork, and two workarounds

**The fork.** The submodule comes from Hocket's fork, `pastaplsok/tracktion_engine`, branch `bugfix/graph_rebuild_skips_audio_block`: upstream `develop` at `a5e44996f5f`, plus one commit, "Graph: Fixed graph rebuilds making the audio thread skip a block". Without it, a clip rewritten during playback now and then plays a 5.8 ms block of silence, heard as a click (PROGRESS.md, Loose ends: dropped blocks).
- The fork is kept long-term. The fix hasn't been reported or offered upstream, by Lee's decision (PROGRESS.md: Known upstream issues).
- The fork is public, so the modified Tracktion source is available to anyone who gets Hocket, as the GPL requires. The GPL doesn't require sending the change upstream.

**Two workarounds in Hocket's own code**, for Tracktion bugs that aren't fixed upstream either:
- **The first MIDI scan:** `--play` runs Tracktion's first MIDI device scan itself before it starts playing (`src/app/Main.cpp`). Otherwise the scan lands 4 s after startup and stops playback.
- **MIDI clips don't use Tracktion's proxy sequence:** `EditSync` calls `setUsesProxy (false)` on each clip (`src/engine/EditSync.cpp`). With the proxy, clips play through `MidiNode`, which leaves notes stuck when a rewrite changes a sounding note.

### Updating Tracktion
Moving to a newer upstream Tracktion (the 3.5.0 tag when it exists, or later), in one change:
1. **The dropped-block fix: did upstream fix it on its own?** In `modules/tracktion_graph/tracktion_graph/tracktion_LockFreeMultiThreadedNodePlayer.cpp`, check whether `createThreads()` still takes `processMutex` unconditionally, and `prepareToPlay (node, oldGraph, ...)` still calls it for every new graph.
   - **Fixed upstream:** point the submodule back at upstream. Set the URL in `.gitmodules` to `https://github.com/Tracktion/tracktion_engine.git`, run `git submodule sync`, and pin the new upstream commit.
   - **Not fixed:** in the fork, rebase the one commit onto the new upstream commit. Push it as a new branch named after that version, and pin it. Leave the old branch in place, so older Hocket commits can still fetch their pin.
2. **The first MIDI scan: still needed?** In `tracktion_DeviceManager.cpp`, check whether `DeviceManager::initialise()` still calls `rescanMidiDeviceList()` (which starts a 5 ms timer) and then `restartMidiCheckTimer()`, which replaces it with the 4 s rescan timer. Keep the early scan in `--play` until upstream changes that order, or does the first scan straight away.
3. **The proxy: still needed?** In `tracktion_MidiNode.cpp`, check whether `MidiNode::prepareToPlay()` now takes over the old node's sounding notes, and ends the ones the new sequence doesn't have, the way `LoopingMidiNode` does. Keep `setUsesProxy (false)` until it does. To confirm once it might: take the call out, and run the stress test "rewrites that change the notes leave none of them stuck". It fails if notes are left on.
4. **JUCE:** move `external/JUCE` to the new Tracktion's `modules/juce` pin (the JUCE pin note, above).
5. **Check:** run the whole test suite. The engine stress tests fail on a dropped block or a stuck note. Then run `Run-Acceptance.ps1`, with `-FreshClone` if the submodule URL changed. Update this file and PROGRESS.md.

## Bundled inside Tracktion Engine

Tracktion Engine's README lists these bundled libraries (mostly under `modules/3rd_party`). All are permissive:

| Library | License |
|---|---|
| rpmalloc | Public domain |
| moodycamel::ConcurrentQueue | Simplified BSD / BSL-1.0 |
| choc | ISC |
| crill | BSL-1.0 |
| expected (TartanLlama) | CC0-1.0 |
| libsamplerate | BSD-2-Clause |
| rigtorp/MPMCQueue | MIT |
| magic_enum | MIT |
| farbot | MIT |
| doctest | MIT |

Also in `modules/tracktion_engine/3rd_party`, not in the README list:

| Library | License | Notes |
|---|---|---|
| airwindows | MIT (upstream repo) | The vendored files carry only a copyright line, no license text. |
| signalsmith-stretch | MIT | Off unless `TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1`. |
| SoundTouch | LGPL-2.1-or-later | Off unless `TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1`. |

## Bundled inside JUCE

| Component | License | Used for |
|---|---|---|
| Steinberg ASIO SDK headers (`modules/juce_audio_devices/native/asio`) | Dual: Steinberg ASIO License or GPLv3 — Hocket uses it under **GPLv3** | ASIO device support (`JUCE_ASIO=1`) |

No separate ASIO SDK download is needed.
