# SPEC.md — Hocket

> **Hocket** (n.): a medieval technique where one melody is split between two voices, each playing while the other rests. Two players building one line together.

> Status: **Draft v0.3** — 2026-10-29
> Authors: Lee + Claude
> Sections marked **TBD** are still open and will be filled in as design discussions continue.

---

## 1. Vision

A digital audio workstation designed from the ground up so that a human producer and an LLM (Claude, or a local model) can make music **together**, as co-producers.

Existing DAWs bolt AI on after the fact: they were built for mouse clicks, with binary project files and partial scripting APIs. This DAW treats AI collaboration as a core design constraint:

- The project is **plain text** that a model can read, reason about, and edit precisely.
- Every action — from the GUI, a script, or an AI agent — goes through **one command system**, so all of them are equal, undoable, and conflict-free.
- Models get **analysis tools** to "listen" to the music they help write (the "Beethoven toolkit"), closing the loop between writing and hearing.

The primary use case is **human + AI co-production with a full GUI**. Fully autonomous AI composition is a supported side effect, not the focus.

---

## 2. Core decisions

| Area | Decision | Notes |
|---|---|---|
| Primary platform | **Windows** first | Linux and macOS later, once the product is solid. Use cross-platform libraries from day one so the ports aren't rewrites. |
| Licensing | **AGPL-3.0** | JUCE 8's open-source option is AGPLv3; Tracktion Engine is GPLv3, which may be combined with AGPLv3 code. Licensing Hocket itself as AGPLv3 matches JUCE and avoids mixed-license ambiguity. Every new dependency must be AGPLv3-compatible. |
| Language | **C++** | Most mature ecosystem for real-time audio and VST3 hosting. |
| Audio framework | **JUCE** | Device I/O (ASIO + WASAPI on Windows; JACK/PipeWire/CoreAudio later), plugin hosting, GUI toolkit. |
| GUI | **Native desktop app (.exe), JUCE-drawn** | No browser. Performance-critical views (arrangement, piano roll, waveforms, meters) are custom-drawn in JUCE. JUCE's WebView support remains an option later for secondary panels (e.g. sound browser, settings) if useful. |
| Engine | **Tracktion Engine** (on top of JUCE) | Playback graph, timeline, recording, rendering. We do not write an audio engine from scratch. Undo lives in Hocket's command bus, not Tracktion (§3.3). |
| Recording | **In scope** | Lee records vocals; audio recording and take comping are part of the core design, not an afterthought. |
| MIDI/sequencing | **In scope** | Piano roll, clips, native instruments, MIDI import/export. |
| Third-party plugins | **VST3** (via JUCE) | Phase 2. CLAP evaluated in Phase 2 (JUCE does not host CLAP natively). |
| Scripting | **Embedded Lua** | For generators and user scripts. v1.x. |
| AI integration | **Live API + MCP server** | See §6. |

---

## 3. Architecture overview

```
 ┌──────────┐   ┌──────────────┐   ┌──────────────┐
 │   GUI    │   │ Lua scripts  │   │  MCP server  │ ◄── Claude / local LLM
 └────┬─────┘   └──────┬───────┘   └──────┬───────┘
      │                │                  │  (localhost API, token auth)
      └────────────────┼──────────────────┘
                       ▼
              ┌─────────────────┐
              │   Command bus   │  validate → apply → record undo step (tagged by author)
              └────────┬────────┘
                       ▼
              ┌─────────────────┐       ┌──────────────────────┐
              │  Project model  │ ◄───► │ Text format (de)serializer │ ◄──► project folder on disk
              └────────┬────────┘       └──────────────────────┘
                       ▼  (lock-free handoff, never blocks)
              ┌─────────────────┐
              │  Audio engine   │  Tracktion Engine + JUCE → ASIO/WASAPI
              └─────────────────┘
```

### 3.1 Everything is a command
- The GUI, Lua scripts, and the MCP server are all **clients** of the same command bus. None has special powers.
- Every command is validated before it applies. Invalid commands return a clear, human-readable error (important for LLM self-correction).
- Every applied command becomes part of undo history, **tagged with its author** (`Lee`, `Claude`, `local-model`, `script:<name>`).
- Multi-step operations can be grouped into a **transaction** that appears as a single named undo step (e.g. "Claude: wrote bridge").
- **Continuous gestures coalesce.** A knob drag or fader move is one undo step, not one step per value change.
- **Transport actions** (play, stop, seek, loop on/off) also go through the command bus, but they don't change the project, so they're not recorded in undo history.

### 3.2 Real-time safety
- The audio thread is never touched directly by any client.
- Commands apply on the main/message thread; state changes are handed to the engine without locks or blocking.
- Requirement: glitch-free playback even while an AI agent is issuing rapid commands.

### 3.3 Source of truth and the Tracktion layer (known risk)
**Hocket's own project model is the source of truth.** Tracktion Engine has its own internal data model (ValueTree-based, saved as XML) and its own undo system; we treat its `Edit` as **derived state**. A sync layer pushes each applied command's changes from our model into the Edit, with Tracktion's undo disabled. Project state is never read back out of the Edit, and the Edit's XML is never saved as the project.

Why our model and not Tracktion's:
- `.notes` comments and other text-only details (§5) must survive edits, and Tracktion's data model has nowhere to keep them.
- Canonical, byte-identical output (§4.1) is simple when we serialize a model we own, and fragile when re-derived from Tracktion's XML.
- Undo needs author tags and named transactions (§3.1), which Tracktion's undo manager doesn't support.

Keeping the Edit in sync with our model is the area most likely to produce early bugs.
- Requirement: round-trip tests — load text → Hocket model → save text must produce byte-identical output for canonical files.
- Requirement: sync tests — after a command applies, the Edit matches the model (checked by offline render).
- Requirement: property/fuzz tests on the serializer.

**Changes that start inside the engine.** Some state originates in Tracktion or a plugin rather than in a client: recorded takes (v1, §10.1) and parameter changes made in a VST3 plugin's own GUI (Phase 2, §10.3). These must be captured back into the command bus as commands, authored by the user, so they're validated, undoable, and serialized like everything else. Continuous plugin-GUI knob moves are coalesced into one undo step (§3.1). Nothing may change the Edit without a matching change to our model.

---

## 4. Project format

A project is a **folder**, designed to be readable by humans and models, and diff-friendly under git.

```
my-song/
  project.json          # tempo, time signature, tracks, routing, mixer, device chains, clip placements
  clips/
    bass-verse.notes    # note data, one file per MIDI clip
    drums-chorus.notes
  automation/
    pad-cutoff.auto     # automation lanes
  audio/
    vocal-take-03.flac  # recorded/imported audio (never inlined)
  plugins/
    serum-lead.state    # opaque third-party plugin state (never inlined)
```

### 4.1 Rules
1. **JSON for structure** (`project.json`): strict, universally supported, reliably written by models. Validated against a published **JSON Schema**.
2. **Compact text notation for notes** (`.notes`) — see §5. Note data is where size explodes; a JSON object per note would cost ~6× the tokens.
3. **Musical units everywhere**: bars/beats, note names, real parameter units (`"cutoff": "2.4kHz"`) for native devices.
4. **VST parameters** stored as normalized value **plus** the plugin's display string, so a model sees `Attack: 12 ms`, not `Param 4: 0.0347`.
5. **Stable IDs, never list positions.** Every track, clip, device, and send has an ID (`trk-bass`, `dev-pad-filter`). References are by ID.
6. **Canonical formatting.** The DAW always writes files in a fixed order and style so diffs are minimal and meaningful.
7. **Binary stays out of text.** Audio, recordings, and plugin state live in their own files, referenced by relative path.
8. **Schema versioning.** `project.json` carries a format version; the DAW migrates older projects forward.

### 4.2 Automation (`.auto`)
Lanes may be written as **breakpoints or shapes**; the DAW expands shapes internally.
```
# pad-cutoff.auto — target: dev-pad-filter.cutoff
1.1    0.20
9.1    ramp 0.20→0.90 until 17.1 exp
17.1   hold 0.90
```

### 4.3 Audio clips and recording
- An audio clip references a file in `audio/` with offset, length, fade in/out, and gain.
- **Comping:** a clip can hold a list of take references, with the chosen segment of each take marked.
- Recording writes new files into `audio/` and adds take references; it never modifies existing audio files.

---

## 5. Note notation (`.notes`) — draft

Design goal: every line stands on its own. Positions are **absolute**, so a mistake in one note never shifts the notes after it (the main failure mode of sequential formats like ABC). Semantics map 1:1 to MIDI so conversion is lossless.

```
# bass-verse.notes — 4 bars
@length 4bars

1.1      C2    1/8   v96
1.1.3    C2    1/16  v70
1.2      Eb2   1/4   v90
1.4      G1    1/4   v85
2.1      cc74  ramp 40→100 until 3.1     # filter sweep
3.3      pb    +0.5 until 3.4            # pitch bend, in semitones-relative units TBD
```

| Field | Format | Notes |
|---|---|---|
| Position | `bar.beat[.sixteenth][+ticks]` | 1-based, relative to the clip start. `+ticks` allows micro-timing (960 ticks per beat). |
| Pitch | Note name + octave (`C2`, `F#4`, `Bb3`) | Uppercase letter, optional `#` or `b`, octave. `C4` = MIDI 60 (middle C); range `C-1` to `G9`. No key-signature inference — the written name is the pitch, and its spelling (`Eb2` vs `D#2`) is kept. Lowercase names are drum-map names (§5.2). |
| Length | Fraction of a whole note (`1/8`, `1/16`, `3/8`), triplets (`1/8t`), or ticks (`250tk`) | |
| Velocity | `v1`–`v127` | Required on every note line. |
| Controllers | `ccN`, `pb`, aftertouch | Values or shapes, same shape syntax as automation. |
| Comments | `#` to end of line | `#` starts a comment at the start of a line or after whitespace, so the `#` in `F#4` is not one. Preserved on canonical rewrite. |

### 5.1 Decided details
- **Timing resolution:** 960 ticks per beat. `+ticks` offsets are for human feel and micro-timing.
- **Chords:** one note per entry internally. The canonical writer collapses notes sharing position, length, and velocity into bracket form, and the parser accepts either form:
  ```
  1.1      [C3 E3 G3]  1/2   v80
  ```
- **Swing:** a clip- or track-level setting, applied at playback. Notes are stored straight, so edits stay simple:
  ```
  @swing 1/16 58%
  ```
- **MIDI channel:** set per clip in the header (`@ch 2`); an optional per-note `chN` override exists but is rarely needed.
- **Ties / legato:** expressed by note length (overlapping or touching notes); no separate syntax.
- **Ghost notes:** just low velocity (e.g. `v25`). No special syntax.

### 5.2 Drum maps
Drum tracks define a **drum map** in `project.json` that maps names to MIDI notes. Clips on that track use the names:
```
1.1    kick       1/16  v110
1.2    snare      1/16  v100
1.2.3  snare      1/16  v28      # ghost note
1.1    hat-closed 1/16  v80
```
- **Naming rules:** lowercase, hyphenated, unique within the map.
- **Descriptive names beat numbers** where the kit allows it: `crash-left` / `crash-right`, `crash-18`, `china`, `splash`.
- **Articulations are their own entries:** `hat-closed`, `hat-open`, `hat-pedal`, `ride`, `ride-bell`, `snare-rim`, `sidestick`.
- **Default map** follows General MIDI percussion, which numbers duplicates: `crash` / `crash2`, `ride` / `ride2`, `tom-low` / `tom-mid` / `tom-high`, etc. Users or models can rename entries per kit.
- **Fallback:** a note with no map entry is written as its note name (`C#1`), so nothing is ever lost or unreadable.
- Choke groups (e.g. closed hat cutting off open hat) belong to the instrument / drum map, not the notation.

### 5.3 MIDI interop
- MIDI import → readable `.notes` clips. MIDI export of any clip or the whole song.
- Internally, the engine converts `.notes` to MIDI events for instruments and plugins.
- ABC import: possible later extra, not a design driver.
- DAWproject import/export: planned for later, for moving songs between this and other DAWs.

### 5.4 Canonical form
The writer always produces exactly this form. The parser accepts looser input: any spaces or tabs between fields, CRLF line endings, a UTF-8 BOM, lines in any order, and a chord split across several lines.

- **File:** UTF-8, LF line endings, no BOM, no trailing whitespace, ends with one newline.
- **Layout:** header comments, then `@length`, then a blank line, then the note lines, with a blank line between bars. Comments after the last note line come last.
- **Order:** by position. Notes at one position that share length and velocity form one chord line, pitches low to high. Lines at the same position are ordered by their lowest pitch.
- **Columns:** fixed-width and left-aligned: position 9 characters, pitch 6, length 6, then velocity. A field that fills or overflows its column is followed by two spaces. Widths never depend on the rest of the file, so editing one line never reformats another.
- **Position:** shortest form: `bar.beat`, plus `.sixteenth` only when it isn't 1, plus `+ticks` only when they aren't 0 (always under 240).
- **Pitch:** as written, except `Cb`, `Fb`, `B#` and `E#`, which are written as the natural note (`Cb4` → `B3`).
- **Length:** a reduced fraction with a power-of-two denominator up to 64, if there is one (`2/8` → `1/4`); otherwise such a fraction as a triplet (`1/12` → `1/8t`); otherwise ticks (`250tk`).
- **`@length`:** always written, in bars (`@length 1bar`, `@length 4bars`). A file without it gets the smallest whole number of bars that holds every note's start (at least one).
- **Comments:**
  - Comments before the first directive or note line are the file header and stay at the top.
  - Any other full-line comment belongs to the line after it and moves with that line. A trailing comment belongs to its own line.
  - Text is kept exactly, minus trailing whitespace. Full-line comments lose indentation; trailing comments go two spaces after the line.
  - When lines merge into a chord, all their comments are kept: full-line comments in their original order, trailing comments joined with `; `.

Validity rules: a note may not start at or after the clip's end, but may extend past it (cut off at playback). Two notes with the same position and pitch are an error.

---

## 6. AI integration

### 6.1 Source of truth
| DAW state | Source of truth | How AI edits |
|---|---|---|
| **Open** | The running DAW | Through the live API (MCP). |
| **Closed** | The project folder | Directly on files (e.g. Claude Code scaffolds a song from scratch); DAW loads it on open. |

If the project folder is modified externally while the DAW is open, the DAW **detects it and asks** the user whether to reload or merge. It never silently overwrites either side.

### 6.2 The API speaks the text format
Reading a clip returns `.notes` text; writing a clip accepts `.notes` text or a patch. The model's "language" is identical whether working live or on saved files.

### 6.3 Transport
- The DAW exposes a **localhost WebSocket API**. (Chosen over Windows named pipes: easier to debug and not platform-specific.)
- A small **MCP server** bridges MCP clients (Claude Desktop, Claude Code, local-model agents) to that API.
  - The bridge (`hocket-mcp`, C++, stdio transport) is a thin adapter: it reads the token file, connects to the WebSocket API, and forwards tool calls. **Tool definitions live in the DAW**, next to the command bus, so there is one list of tools.
  - Chosen over the DAW serving MCP over HTTP itself: the bridge reads each session's token and port from the file, so a client is registered once rather than every session; stdio is the transport every local MCP client supports; and the bridge can report "Hocket isn't running" as a readable tool error and reconnect when the DAW restarts.
- **Token auth:** the DAW writes a per-session token to a local file; clients must present it. Prevents arbitrary local software from controlling the DAW.

### 6.4 Tool set (initial sketch)
Prefer a small set of expressive tools over many tiny ones.

| Tool | Purpose |
|---|---|
| `get_project_overview` | Tempo, structure, tracks, devices, clip placements — a compact summary. |
| `read_clip` / `write_clip` / `patch_clip` | Note and audio clip contents in text form. |
| `read_automation` / `write_automation` | Automation lanes. |
| `add_track` / `remove_track` / `set_routing` | Track and mixer structure. |
| `add_device` / `set_params` | Device chains and parameter changes (native + VST). |
| `begin_transaction` / `commit` / `rollback` | Group changes into one named undo step. |
| `transport` | Play, stop, loop region, position. |
| `render_section` | Render a time range to audio for analysis. |
| `analyze` | Run analysis tools on a render (see §7). |
| `undo` / `history` | Inspect and revert history, filterable by author. |

### 6.5 Proposal mode
Optional toggle: AI edits arrive as **pending** — shown as ghost notes / highlighted changes — and the user accepts or rejects them, individually or as a batch.

### 6.6 Connecting AI clients (accounts and terms)
- **The DAW is an MCP server; AI tools are external clients.** Claude Code or Claude Desktop, signed in with the user's own Claude plan, connects to the DAW's local MCP server like any other tool. The DAW never sees, stores, or uses the user's Claude credentials.
- **The DAW must never implement Claude subscription login** or reuse subscription tokens. Anthropic's terms limit those to Claude Code and Claude.ai.
- **Any built-in AI feature** (e.g. a future in-app assistant panel calling a model directly) uses **bring-your-own API key** from Claude Console, or a local model (e.g. Ollama). This matters especially since the DAW is distributed as open source.
- Local models connect the same way as Claude: through the MCP server.

### 6.7 Embedded Lua
- Lua scripts are command-bus clients with the same API surface as MCP.
- Primary use: **generators** ("walking bassline over these chords"), batch edits, custom tools.
- A model can write a Lua generator instead of placing every note by hand.

---

## 7. Analysis toolkit ("Beethoven toolkit")

The model can't hear; these tools let it check its work. Analysis is **built into the DAW**, using free AGPLv3-compatible libraries plus our own summarizing layer. Third-party meters (e.g. MiniMeters) remain great for the human producer, but they don't expose readings to the host as data, so they can't serve the model.

### 7.1 Measurements and libraries
| Measurement | Source | License |
|---|---|---|
| Spectrum / frequency balance | JUCE FFT | (JUCE) |
| Loudness: integrated/short-term LUFS, true peak, crest factor | libebur128 | MIT |
| Stereo width, phase correlation | Own code | — |
| Tempo, onsets, pitch | aubio | GPLv3 |
| Key detection | libKeyFinder, or Essentia | GPLv3 / AGPLv3 — both compatible with Hocket's AGPLv3 license |
| Frequency masking between tracks | Own code (built on per-track FFT) | — |

### 7.2 Output design: summarized, musical, located
The toolkit never dumps raw data (e.g. FFT bins) to the model by default. Results are **short text summaries** tied to bars and tracks, with concrete numbers, so the model can act on them immediately:

```
analyze bars 9–16, tracks [kick, bass]
→ masking: kick & bass overlap 45–90 Hz; bass is ~4 dB louder at 60 Hz on kick hits
→ master: -9.2 LUFS short-term, true peak -0.3 dBTP (close to clipping)
→ stereo: bass correlation 0.98 (mono — good); pad very wide, correlation 0.21
```

Raw data remains available on explicit request (e.g. for a script).

### 7.3 Features
- **Render & analyze** any time range, per track, per bus, or master. Offline render, so it doesn't depend on real-time playback.
- **Before/after comparison:** analyze a section, apply an edit, re-analyze, and report what actually changed. This is how the model verifies its own edits.
- **Reference tracks:** the user loads a commercial track; the toolkit compares tonal balance, loudness, and stereo width between the reference and the mix, giving the model a concrete target.
- **Images as a secondary channel:** optional spectrogram / waveform renders as images for multimodal models (useful for spotting arrangement-level issues like muddy sections). Text summaries remain primary, since not all models, including some local ones, accept images.

### 7.4 Open details
- Exact summary vocabulary and thresholds (what counts as "masking", "too wide", "close to clipping")
- Whether genre-aware targets are useful (e.g. typical loudness ranges by genre) or left entirely to reference tracks

---

## 8. Instruments and plugins

### 8.1 Two tiers
1. **Native devices** (synths, sampler, effects) — every parameter documented, named in real units, and fully readable in the text format. These are where AI collaboration is strongest ("the pad is muddy, lower the cutoff and shorten the release").
2. **Third-party VST3 plugins** — treated as **black boxes with labeled knobs**. Parameters are exposed and automatable; state is an opaque blob.

### 8.2 VST3 hosting requirements (Phase 2)
- Expose parameters with names, normalized values, and display strings.
- **Parameter mapping files** per plugin to rename cryptic parameters (`Macro 3` → `Filter Drive`), built up over time and shareable.
- Save/load/swap plugin state and presets.
- **Out-of-process sandboxing** so a crashing plugin doesn't take down the DAW.
- Latency reporting and compensation.

### 8.3 Test plugin set (all free/legal)
Surge XT, Vital (free tier), Dexed, Airwindows, Valhalla Supermassive, TDR Nova, MeldaProduction free bundle, Cardinal, Steinberg VST3 SDK example plugins. MiniMeters (already owned by Lee) as a commercial test case. Commercial plugins (incl. copy protection like iLok) tested via official demos. Serum 2 is the one planned purchase, once VST hosting exists.

---

## 9. Platform notes
- Windows first: ASIO and WASAPI via JUCE. (ASIO SDK is available under a GPLv3 option; VST3 SDK is MIT since 3.8.)
- Keep platform-specific code isolated so Linux (JACK/PipeWire) and macOS (CoreAudio, AU) ports are additive.

---

## 10. Scope and roadmap

### 10.0 Milestone 0 — headless proof (before any GUI work)
A command-line program that:
1. Loads a project folder (`project.json` + one `.notes` clip),
2. Plays the clip through a native synth over ASIO,
3. Lets an MCP client rewrite that clip **while it plays**, with the change landing as an undoable command.

Purpose: prove the riskiest architecture pieces work together early — text format, command bus, Tracktion sync, and real-time safety.

The native synth for Milestone 0 is Tracktion's built-in 4OSC, as a stand-in; Hocket's own synth comes in v1 (§10.1).

Milestone 0 `.notes` subset: note lines, chords, comments and `@length`, in 4/4 only. Controller lines (`cc`, `pb`, aftertouch), `@swing`, `@ch`, per-note `chN` and drum names are rejected with an error saying they aren't supported yet.

### 10.1 v1 — "Lee and Claude can make a complete song in it, start to finish, with native instruments"
- Engine on Tracktion; ASIO/WASAPI; transport and looping; one tempo and time signature per song
- MIDI tracks, piano roll, `.notes` format, MIDI import/export
- Native devices: one solid synth, a sampler/drum sampler, EQ, compressor, reverb, delay, filter/saturator
- Audio tracks: audio import and **recording** (arm a track, record takes — vocals are a v1 requirement)
- Mixer: volume, pan, sends, master bus; basic automation
- Command bus, undo with author tags, transactions
- Project format with canonical writer and round-trip tests
- MCP server with the core tool set (§6.4)
- Analysis toolkit core: loudness, spectrum, masking, before/after comparison
- Export to WAV/FLAC, including **seamless loop export** (for game music) and per-track stem export
- GUI: arrangement view, piano roll, mixer — functional rather than polished, but built on a **theming/style token system from day one** so Phase 2 polish is restyling, not rewriting

### 10.2 v1.x
- Take comping, punch-in recording
- Embedded Lua scripting (the command bus is designed scripting-ready in v1; Lua is a power-up, since models can already write clips directly via the API)
- Proposal mode (ghost notes, accept/reject)
- Reference track comparison
- Tempo and time signature changes within a song

### 10.3 Phase 2
- VST3 hosting, out-of-process sandboxing, parameter mapping files (Serum 2 purchase happens here)
- **GUI polish:** dedicated focus on layout, visual style, and approachability. Goal: powerful like REAPER, but welcoming on first launch rather than intimidating.

### 10.4 Later
- Audio warping / time-stretching
- DAWproject import/export
- Linux and macOS
- CLAP, MPE, notation view

---

## 11. Open questions
- Analysis toolkit summary vocabulary and thresholds (§7.4) — tune against real renders during v1
- CLAP support (Phase 2)
- Name check: confirm "Hocket" is clear on GitHub, domains, and trademarks before going public
