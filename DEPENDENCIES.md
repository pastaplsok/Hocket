# Dependencies

Hocket is AGPL-3.0. Every dependency must be AGPLv3-compatible (see CLAUDE.md). Add a row here in the same change that adds a dependency.

All source dependencies are git submodules under `external/`, each pinned to an exact commit. Clone with `git submodule update --init` (**not** `--recursive`; see the JUCE note below). On Windows, clones deeper than about 95 characters hit the 260-character path limit on a few JUCE files under `extras/` (not part of the build); `git config --global core.longpaths true` fixes it.

| Dependency | Pin | License | Used by |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | `7ff644f65d7daa1428c34b9738ebb5c449dc9172` (develop, after 9.0.4) | AGPLv3 | engine, app |
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) | `a5e44996f5f08dfa6af5f5d02d900fb3cfa37422` (develop, 3.5.0) | GPLv3 | engine |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | `v12.1.0` | BSD-3-Clause | API server, MCP bridge |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` | MIT | core, API server, MCP bridge |
| [doctest](https://github.com/doctest/doctest) | `v2.5.3` | MIT | tests |

**JUCE pin.** JUCE is our own top-level submodule, pinned to the same commit Tracktion Engine's `modules/juce` submodule points at. We don't use Tracktion's nested submodule because its URL is SSH-only (`git@github.com:`), which fails for anyone without GitHub SSH keys. CMake warns at configure time if the two pins drift apart; when updating Tracktion, move JUCE to Tracktion's new pin in the same change.

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
