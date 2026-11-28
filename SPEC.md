# SPEC.md — Hocket

> **Hocket** (n.): a medieval technique where one melody is split between two voices, each playing while the other rests. Two players building one line together.

> Status: **Draft v0.30** — 2026-11-27
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
| Recording | **In scope** | Lee records vocals; audio recording and take comping are part of the core design, not an afterthought. MIDI recording from a keyboard is in v1 too. |
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
- Multi-step operations can be grouped into a **transaction** that appears as a single named undo step (e.g. "Claude: wrote bridge"). Its name is one line of text, at most 100 bytes, since the history lists one entry per line.
- **One linear undo history**, shared by every client. Undo always takes back the newest entry. Entries have stable numbers (`#7`).
- **AI undo checks the author.** The MCP `undo` tool refuses when the newest entry isn't the caller's, with a message naming who made that change and what it was (e.g. `the most recent change, #7 "wrote clip clp-bass (14 notes)", was made by Lee, not Claude`). Passing `force` undoes it anyway. Undo from the GUI works normally, whoever made the change. The MCP `redo` tool checks the same way, against whoever did the undo it would take back.
- **Authors are compared by identity: name + session ID.** The MCP bridge generates a session ID at startup and keeps it across reconnects to the DAW, so two Claude sessions are different authors, and a client that reconnects is still the author of its own entries. Display names stay friendly ("Claude"), optionally with a label the client provides when it connects (§6.3); when two authors share a name, messages say which one made the change ("Claude in another session", "Claude (drums)"). The person at the DAW has one fixed identity; `hocket-headless` takes its display name from `--user`.
- **Transactions and other authors.** One transaction can be open at a time. A command from another author never waits for it: the transaction's edits so far become their own history entry ("wrote bridge (part 1)"), the other command follows, and the transaction carries on as part 2. History stays linear, and every entry can be undone on its own. Rolling back takes back only the current part.
- **Interrupted transactions are kept, not lost.** If the client that opened a transaction disconnects, the bus commits what the transaction holds as an entry marked "(interrupted)" (`wrote bridge (interrupted)`, or `wrote bridge (part 2, interrupted)` after a split). The entry keeps the transaction's author, so it can be undone like any other, including by that client when it reconnects. A transaction with no edits just closes. An **idle timeout** is the backstop for a client that stays connected but stops sending: it closes the transaction the same way. It counts from the owning client's last request of any kind and is **10 minutes**, long enough to cover a model thinking between calls. Erring long costs little: an open transaction never makes anyone's single edits wait. The client's next tool result says what happened to its transaction, even after a reconnect.
- A command that changes nothing (e.g. writing a clip's current text again) adds no history entry. Clips are compared in canonical form (§5.4): text that the writer would write out the same as the clip changes nothing, however its lines are laid out or ordered.
- **Continuous gestures coalesce.** A knob drag or fader move is one undo step, not one step per value change.
  - A command can carry a gesture ID. Changes in a row with the same ID, from the same author, to the same setting become one history entry, which undoes to the value before the first of them.
  - A gesture that ends where it started leaves no entry. Undo, redo, another author's edit, or a change to another setting ends the gesture. Inside an open transaction, its steps are simply part of the transaction's one entry.
  - The tempo and a track's mix take gestures; a change to several mix settings at once is one setting, so dragging volume alone and then volume with pan are two gestures.
  - There's no start or end call, so a client that disconnects mid-gesture leaves nothing open.
- **Transport actions** (play, stop, seek, the loop region, loop on/off) also go through the command bus, but they don't change the project, so they're not recorded in undo history. The bus keeps their settings for the session and checks each one (positions are read in the song's time signature, and a seek against the song's end), then hands it to the engine to carry out. They don't split another author's open transaction, since they aren't edits.
  - Since they can't be undone, their results say what the previous value was ("seeked to 9.1, from 3.2", "loop region 5.1 to 9.1 (4 bars), was the whole song").
  - Their positions are kept in ticks, so a tempo or time-signature change leaves them at the same place in the music, as it does clips.
  - **Seek** takes a song position ("9.1"). Play starts there from then on, until the next seek; stop doesn't move it, so play, stop, play starts at the same place each time. While playing, the playhead also jumps there: the notes sounding end, and those on at the new place are struck. A position at or past the song's end is refused, unless it's in the loop region.
  - **The loop region** lasts the session: it's transport state, not saved in the project. It may run past the song's last clip (to loop bars that are about to be written or recorded), but not past bar 999, and it's at least a sixteenth note long. With one set, play loops it; without, play loops the whole song. When one is set and the playhead is outside it, the playhead wraps into it as in §3.3, with loop on or off: counted from the region's start, which keeps its place in the bar when the region is whole bars. When stopped, play starts at the region's start, unless the last seek is inside it.
  - **A note already sounding where play starts or a loop begins sounds from there**, struck part-way through: one under the playhead when play starts or a seek lands, and, at every pass of a loop, one that starts before the loop and runs into it (Lee, 2026-11-27). A setting to turn this off, "chase notes", is for the GUI (Milestone 2). A wrap the engine makes after a change doesn't strike such a note (§3.3); the passes after it do.
  - **Loop off:** play runs to the end of the song (or the region) and stops. Nothing after the end is heard, though a region's end is followed by more of the song: the notes sounding there end at it and ring out, and none after it starts, even if the stop itself comes a few milliseconds later.

### 3.2 Real-time safety
- The audio thread is never touched directly by any client.
- Commands apply on the main/message thread; state changes are handed to the engine without locks or blocking.
- Requirement: glitch-free playback even while an AI agent is issuing rapid commands. Checked by stress tests that rewrite a clip 20 times a second while it plays in real time, through the same path a sound card's callback takes. A block over its time budget, a dropped block, a gap, a click, a retriggered note or a stuck one fails them.

### 3.3 Source of truth and the Tracktion layer (known risk)
**Hocket's own project model is the source of truth.** Tracktion Engine has its own internal data model (ValueTree-based, saved as XML) and its own undo system; we treat its `Edit` as **derived state**. A sync layer pushes each applied command's changes from our model into the Edit, with Tracktion's undo disabled. Project state is never read back out of the Edit, and the Edit's XML is never saved as the project.

Why our model and not Tracktion's:
- `.notes` comments and other text-only details (§5) must survive edits, and Tracktion's data model has nowhere to keep them.
- Canonical, byte-identical output (§4.1) is simple when we serialize a model we own, and fragile when re-derived from Tracktion's XML.
- Undo needs author tags and named transactions (§3.1), which Tracktion's undo manager doesn't support.

Keeping the Edit in sync with our model is the area most likely to produce early bugs.
- Requirement: round-trip tests — load text → Hocket model → save text must produce byte-identical output for canonical files.
- Requirement: sync tests — after a command applies, the Edit matches the model (checked by offline render), and a change made during playback is heard without stopping or retriggering held notes (checked by playing through a simulated audio device). A note the change takes away while it's sounding ends then (on the drum sampler, whose pieces are one-shots, its sound rings on, §5.2), and no rewrite, however fast, leaves a note on; a note the change adds starts the next time the playhead reaches its start, not part-way through. That holds on every instrument, whether the note is added to a clip that's playing or comes with a clip that's added, moved, or brought back by undo (Lee, 2026-11-25). Like any change, it takes a moment to reach the audio, a few milliseconds and a block or two (two more when another change came just before, below), so a note starting within that moment starts on the next pass.
- Changes made during playback reach the engine together, at most one batch for each block played: a change that comes before the engine has played the last batch waits for it, and goes in with any others that came meanwhile (Lee, 2026-11-27). So several requests inside one block, even one client's in one turn, leave no note on. A command's own changes always go in together.
- Requirement: property/fuzz tests on the serializer.
- When a change during playback ends the song before the playhead (the last clip removed while it plays, say), playback wraps as if the loop had always been that length, landing on the same beat of the bar so the pulse carries on, rather than jumping to 1.1. A note that would be sounding at the new place isn't struck: like any note, it starts when the playhead next reaches its start (Lee, 2026-11-25; until then a wrap struck it). The same goes for a wrap into a new loop region (§3.1). That's while looping: with loop off, playback stops instead, since the song played once has reached its end.
- A tempo change during playback keeps the playhead on its beat, so its place in the bar, and the notes sounding carry on. A new time signature keeps the playhead at the same time, as it keeps every clip and note (§4.4).
- While a loop region plays once (loop off), the Edit's clips end at the region's end, and those that start at or after it are left out, so nothing after it is heard before the transport stops (§3.1). Every way out of playing once (reaching the end, stop, loop on, a new region or none) gives the clips their own ends back. The sync tests compare the Edit with the model cut off in the same place, worked out on their own.

**What Milestone 0 found in Tracktion.** Its stress tests found two Tracktion bugs that only show when a clip changes many times a second during playback. Both were invisible at ordinary editing speed, which is exactly the speed an AI client doesn't keep to.
- Notes left on for good: a MIDI clip that uses Tracktion's proxy sequence (the default) keeps no record of sounding notes when its graph is rebuilt. Hocket's clips don't use the proxy.
- Now and then, a whole block of silence: a graph rebuild took the lock the audio thread needs. Hocket builds Tracktion from its own public fork, which carries the fix (DEPENDENCIES.md).

**What a note starting only at its start takes in Tracktion** (bug sweep, area 2). Tracktion's player for a clip strikes the notes the clip has sounding where the playhead is when the player is new (a clip added, or remade by a move) or the playhead jumps (a wrap, a seek), and it builds each playback graph against the last one it built, played or not, so what one graph was to do once (end the notes a rewrite took away, or those of a clip taken out) is lost if another is built over it before it plays. So during playback the sync layer has Tracktion build a graph only once the last one has played a block, and the changes that come before then wait and go in together; a seek, a play or a wrap builds at once, since the jump that follows ends every note sounding, so nothing is lost. It leaves out of the Edit the notes that would be struck part-way through, and puts them back once a block has played without them; and it empties a clip it takes out, so its own player ends its notes, and takes it out once a block has played so. A seek and play still strike the notes sounding where they start (§3.1).

So the sync layer's tests play in real time, not just render offline, and a new kind of change to the Edit comes with a stress test of its own. Neither bug has been reported upstream; each Tracktion update checks whether upstream has fixed them on its own (DEPENDENCIES.md, "Updating Tracktion").

**Changes that start inside the engine.** Some state originates in Tracktion or a plugin rather than in a client: recorded takes (v1, §10.1) and parameter changes made in a VST3 plugin's own GUI (Phase 2, §10.3). These must be captured back into the command bus as commands, authored by the user, so they're validated, undoable, and serialized like everything else. Continuous plugin-GUI knob moves are coalesced into one undo step (§3.1). Nothing may change the Edit without a matching change to our model.

---

## 4. Project format

A project is a **folder**, designed to be readable by humans and models, and diff-friendly under git.

```
my-song/
  project.json          # tempo, time signature, tracks, routing, mixer, device chains, clip placements
  clips/
    bass-verse.notes    # note data: one file per MIDI clip, or per set of linked clips
    drums-chorus.notes
  automation/
    pad-cutoff.auto     # automation lanes
  audio/
    vocal-take-03.flac  # recorded/imported audio (never inlined)
  plugins/
    serum-lead.state    # opaque third-party plugin state (never inlined)
```

### 4.1 Rules
1. **JSON for structure** (`project.json`): strict, universally supported, reliably written by models. Validated against a published **JSON Schema** (comes in v1; until then the loader's validation, §4.4, is the reference).
2. **Compact text notation for notes** (`.notes`) — see §5. Note data is where size explodes; a JSON object per note would cost ~6× the tokens.
3. **Musical units everywhere**: bars/beats, note names, real parameter units (`"cutoff": "2.4kHz"`) for native devices.
4. **VST parameters** stored as normalized value **plus** the plugin's display string, so a model sees `Attack: 12 ms`, not `Param 4: 0.0347`.
5. **Stable IDs, never list positions.** Every track, clip, device, and send has an ID (`trk-bass`, `dev-pad-filter`). References are by ID.
   - **IDs are permanent:** assigned once and never changed, even when the display name changes. Names are free text and can change at any time.
   - **Format:** a type prefix (`trk-`, `clp-`, `dev-`), then lowercase letters and digits in hyphen-separated words, at most 64 characters in all, the prefix included. Unique across the whole project, so an ID always names exactly one thing.
   - **New IDs:** a client adding a track or clip may choose its ID; otherwise Hocket makes one, and the result says which. A track's comes from its name (`trk-drums`, then `trk-drums-2`); a new clip's from its track's (`clp-drums`, then `clp-drums-2`); a copy's or a linked clip's from the clip it came from (`clp-verse` → `clp-verse-2`, `clp-verse-2` → `clp-verse-3`). A device's ID comes from its track's (`dev-drums-kit`).
6. **Canonical formatting.** The DAW always writes files in a fixed order and style so diffs are minimal and meaningful.
7. **Binary stays out of text.** Audio, recordings, and plugin state live in their own files, referenced by relative path.
8. **Schema versioning.** `project.json` carries a format version; the DAW migrates older projects forward. Version 0 is the pre-release format: it may change without migrations until v1, which starts at version 1.

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

### 4.4 `project.json`
```json
{
  "format": "hocket-project",
  "formatVersion": 0,
  "tempo": 120,
  "timeSignature": "6/8",
  "tracks": [
    {
      "id": "trk-bass",
      "name": "Bass",
      "type": "midi",
      "volume": "-6dB",
      "pan": "L20",
      "mute": false,
      "solo": false,
      "devices": [
        {
          "id": "dev-bass-synth",
          "type": "hocket:synth"
        }
      ],
      "clips": [
        {
          "id": "clp-bass-verse",
          "file": "clips/bass-verse.notes",
          "start": "1.1"
        },
        {
          "id": "clp-bass-verse-2",
          "file": "clips/bass-verse.notes",
          "start": "9.1"
        }
      ]
    }
  ]
}
```
- **Strict:** every key shown is required. Unknown keys and duplicate keys are errors, so a typo is never silently ignored.
- **Order:** `tracks` is in display order, `devices` in signal-chain order.
- **Track names** are one line of text: 1 to 100 bytes of UTF-8, with no control characters (so no line breaks or tabs), since lists show one track per line. Otherwise they're free text.
- **Tempo** is in quarter notes per minute, whatever the time signature: 20–300. Tracktion, which plays the song, goes no faster. A faster feel is written at half the tempo with notes half as long (400 BPM in quarter notes sounds the same as 200 BPM in eighth notes), and the error for a tempo over 300 says so.
- **Time signature:** one for the whole song, `"<top>/<bottom>"`. The top number is 1–32; the bottom one is 2, 4, 8 or 16. **A beat is the bottom number's note**: a quarter in 4/4, an eighth in 6/8 and 7/8, a half in 3/2. Positions (§5) count these beats, and their third number counts sixteenths within the beat: 1–4 in x/4, 1–2 in x/8, 1–8 in x/2, and only 1 in x/16. Ticks are 960 per quarter note in every signature, and note lengths stay fractions of a whole note. (Tracktion's BPM is quarter notes per minute too. Hocket keeps the Tracktion Edit's own time signature at 4/4, so Tracktion's beats are quarter notes, and works out bars itself; anything that reads the signature or the bar from Tracktion, like a metronome or a hosted plugin (§8.2), needs that revisited first.)
  - **Changing it** keeps every clip and note at the same time. Since `@length` is in bars, the change is refused if any clip wouldn't then be a whole number of the new bars, or the song would pass 999 of them; the message lists those clips and lengths that would work. So 4/4 to 2/4 or 8/8 always works, and 4/4 to 3/4 works on an empty song or when the clips fit. When a change goes through and leaves clips starting off a bar line, the result lists them.
- **Clips:** `start` is a song position in `.notes` position syntax (§5). `file` is `clips/<name>.notes`, with a lowercase, hyphenated name; a new clip's file is named after its ID (`clp-verse-2` → `clips/verse-2.notes`) unless the client names one. A name Windows keeps for a device (`con`, `prn`, `aux`, `nul`, `com0`–`com9`, `lpt0`–`lpt9`) is never made: `clp-aux` gets `clips/aux-2.notes`, since Windows 10 and earlier take `aux.notes` for the AUX device, whatever its extension. A clip's length comes from its `.notes` file (`@length`), never from `project.json`, so resizing a clip is a `write_clip` with a new `@length`. Clips on one track may not overlap. A song is at most 999 bars long: every clip ends by 1000.1, so a typo in a start can't make a loop thousands of bars long.
- **Linked clips:** several clips may name the same `.notes` file, pattern-style as in FL Studio. They share its notes and its `@length`, so editing one edits them all (`write_clip` says which others changed). Hocket reads each file once and writes it once. A file linked into drum tracks must mean the same notes on each (§5.2).
  - A linked clip can be made unique: it gets its own copy of the file.
  - When no clip uses a file any more, its notes leave the project, and undo brings them back.
- **Mix:** each track's `volume`, `pan`, `mute` and `solo`, always written.
  - **Values with units** are a number and then the unit: an optional sign, digits, and an optional decimal point with digits after it. The writer puts no space between number and unit; the reader accepts one. Units are case-sensitive (`dB`, not `db`). Device parameters use the same syntax from Milestone 4 (`"2.4kHz"`, `"12ms"`).
  - **`volume`** is in dB, from `"-96dB"` to `"+6dB"` in steps of 0.1 dB (finer values are rounded), or `"-infdB"` for silence. `"0dB"` leaves the signal as it is. The writer drops trailing zeros and puts `+` on boosts: `"-6.5dB"`, `"0dB"`, `"+3dB"`. The top is Tracktion's fader's.
  - **`pan`** is `"C"`, `"L1"` to `"L100"`, or `"R1"` to `"R100"`. **It's a balance control:** at C both channels pass unchanged. Panning towards one side leaves that side's channel unchanged and lowers the other linearly in gain, to silence at L100 or R100: at L50 the right channel is at half gain (−6 dB). So `"0dB"` and `"C"` together leave a track exactly as its instrument made it, all the way to a render: Tracktion starts an Edit's master at −3 dB, and Hocket sets it to 0 dB. Pan never moves one channel's content to the other side: at L100, anything only in the right channel is silent. (Tracktion's own pan laws either boost the near side or lower the centre, so Hocket plays the mix with a gain stage of its own on each track, which takes both channels' gains as one value and ramps them together over 15 ms. Tracktion's volume plugin, which every track has, stays at unity.)
  - **`mute` and `solo`** are `true` or `false`. While any track is soloed, only soloed tracks are heard; a muted track is silent even when soloed.
- **Track types:** `"midi"`, whose clips use note names, or `"drums"`, whose clips use the names in its `drumMap` (§5.2). A track's type is set when it's added and doesn't change in v1; moving its clips to a new track of the other type does the same job. A drums track:
  ```json
  {
    "id": "trk-drums",
    "name": "Drums",
    "type": "drums",
    "volume": "0dB",
    "pan": "C",
    "mute": false,
    "solo": false,
    "devices": [
      {
        "id": "dev-drums-kit",
        "type": "hocket:drum-sampler",
        "kit": "acoustic"
      }
    ],
    "drumMap": {
      "kick": 36,
      "snare": 38,
      "hat-closed": 42
    },
    "clips": []
  }
  ```
  - `drumMap` is required on a drums track and an error on any other. It maps names to MIDI note numbers, 0 to 127. Numbers, not note names, since drum charts disagree about octave names (Hocket's kick is C2, many charts' C1), while General MIDI's numbers are the same everywhere.
  - One note per name, and one name per note, so the writer knows which name to write. A map may be empty.
- **Devices:** none, or one instrument: `"hocket:synth"` (§8.1), or `"hocket:drum-sampler"` with its `kit` (§8.4), on drums tracks only. A bundled kit's name is lowercase and hyphenated, like a clip file's; a kit of the user's own is written `user/` and its name (`"user/dusty-909"`), so the two can never clash (§8.4). A project reads and validates without its kit present, since the names are in the drum map. Effects come in Milestone 5.
- **Canonical form:** keys in the order shown, 2-space indent, one value per line, UTF-8 with LF line endings and a final newline. A drums track's `drumMap` comes between `devices` and `clips`, sorted by note, and a drum sampler's `kit` after its `type`. Clips on a track are sorted by start; tracks and devices keep their order. Whole-number tempos are written without a decimal point.

### 4.5 Saving
**Saving is explicit, keeps every earlier save, and has an automatic recovery copy** (planned with Lee on 2026-11-20).
- Save is a command any client can send: the GUI, MCP's `save`, or the `S` key in `hocket-headless --play`'s console, which saves as the person. When a save is refused because files changed outside Hocket, the console says which, and offers a key of its own, `O`, for saving over them (with `overwrite`, below): only straight after that refusal and while the project is as it was then, and never a second press of `S` (Lee, 2026-11-21). Any other key leaves them as they are. `O` counts only a second or more after the refusal, so a word typed into the console ("sort") can't answer it; and it saves over only what the refusal named, as it was then: if more has changed outside Hocket since (another file, or one of those again), it's refused again, naming what's changed now (Lee, 2026-11-22). Like transport, it isn't a history entry, so it doesn't split an open transaction; a transaction's edits so far are saved, since they're in the project. Hocket knows whether anything is unsaved, from the history: undoing or redoing back to the saved state counts as saved, while a change put back by hand counts as unsaved.
- Each file is written atomically (to a temporary file beside it, then swapped in), clip files before `project.json`, so a crash mid-save never leaves `project.json` naming a missing clip. Every changed file's new text is written beside it before any is swapped in, and a swap that fails (another program holding the file, say) puts back the files already swapped, so a save that fails leaves the folder as it was, and says why. Only files whose text changed are written.
  - **A save a crash interrupts is finished, all of it or none, never left half-done** (Lee, 2026-11-22; all or nothing, 2026-11-24). Once every new text is beside its file and the version it replaces is kept, the save puts `.hocket/saving.json` in place (written beside it first, so it's whole or not there), naming the kept save that holds the version before it, the files that go in place, in order, with each one's stamp as the save found it and as it wrote it, and where in `unused/` each clip file no clip uses goes; it removes it once they're all in place. The next `--play` finishes a save it finds that way, before reading the folder, so the folder ends as the save meant, never part old and part new. It does so only when the marker reads and parses, every file still to put in place is beside its place as the save wrote it, every file already in place is as the save put it, and every file the save replaces is as the save found it and in the kept save. Otherwise it changes nothing on disk, `--play` stops, and it says why, where the version before the save is (the kept save) and where the save's is (the `.hocket-saving` files beside their own, and any already in place). Once the person has taken what they want from them, deleting the marker lets `--play` play the files as they are. The message stopping `--play` gives the full path of `.hocket/saving.json` to delete, says what the next `--play` does then (sets the `.hocket-saving` files aside in `unused/`, keeping their paths, and plays the files as they are, naming any the save had put in place, or says they don't load), and, when a file was only held by another program, says that closing it lets the next `--play` finish the save (Lee, 2026-11-24). A step that fails part-way (a file another program holds open, say) stops `--play` there: what's done stays done, and the next `--play` finishes the rest. A save refuses while an older save's marker is there. A save stopped before its marker changed nothing, and what it left (its temporary files, a part-made copy) is set aside in `unused/`, keeping its path, since its texts may be nowhere else. `--check` changes nothing: it says when a save was interrupted, and whether `--play` can finish it. `--new <folder>` makes an empty project (120 BPM, 4/4, no tracks) in a new or empty folder, making the folder if need be.
- **Unused files:** a clip file that was in the project, but that no clip uses any more, is moved on save to `unused/` (the save's result names what it moved), keeping its path (`unused/clips/verse-2.notes`), and numbered if that name is taken. Nothing is ever deleted, and files Hocket never had in the project are never touched. As a rule, nothing that saves or finishes a save deletes a file whose text exists nowhere else, on disk or in the session whose save wrote it: such a file is set aside in `unused/` (Lee, 2026-11-24). If an undo brings the clip back, the next save writes the file in `clips/` again.
- **Kept saves:** before a save writes anything, it keeps the version it replaces in `.hocket/saves/<number> <time> <author>[ <name>]/`: a complete copy of the project's text files as they were (`project.json` and every clip file), so any one restores by itself, with the revert command or by hand. Every file the save replaces is in it, even when `project.json` itself was deleted outside Hocket (then the copy holds the clip files that were there, and doesn't load as a project). It's read back at its final name before anything is swapped in: one that can't be read (its paths past what Windows allows, say) isn't kept, and the save writes nothing. A save can be named ("before the bridge experiment"; one line, at most 100 bytes). Every save is kept, for now; a cap on how many can come in Milestone 9 (Lee, 2026-11-20).
  - **Who saved a version, and when,** is in `.hocket/save.json` beside it, written by each save: the time (UTC), the author's display name, and the name. It names the version saved, so a kept save carries the name it was saved with. It also lists the version's files (`project.json` and the clip files it names), each with a fingerprint of its text (64 bits of FNV-1a), so a version changed by hand since, while Hocket was closed or by a checkout, is told from the one it describes: it's kept as changed outside Hocket, at its latest change's time, not under that save's author and name (Lee, 2026-11-22). A `save.json` from before the list was added is taken at its word. A kept save's folder has its own `.hocket/save.json`, and is a project folder in its own right. A version Hocket didn't save (made by hand, or checked out) is kept as from before Hocket, at its `project.json`'s date; one with files changed outside Hocket is labelled so (below); and a recovery copy that `--discard-recovery` files here is labelled as a discarded recovery, at the time it was written.
  - A save that changes no file keeps nothing; with a name, it only names the version on disk, which keeps who saved it and when (or its label, if Hocket didn't save it): naming a version doesn't make it the namer's (Lee, 2026-11-22).
  - **Folder names are valid on Windows** whatever the author or the name contains: the characters Windows refuses (`<>:"/\|?*`, control characters) become spaces, invalid UTF-8 is dropped, the time is written `2026-11-20 14-02` (local), trailing dots and spaces go, and the name is cut to 64 UTF-16 units. Starting with the number, it can never be a reserved name like `CON`.
  - **This is the person's way back from an experiment.** Any client can save, an AI client included (Milestone 1 has no GUI, and Claude saves the demo song), so no save may lose a version that was saved: each is kept, and a save that can't keep the version it replaces writes nothing. A revert command restores a kept save into the session as one history entry, which can itself be undone, and a list command shows the kept saves (number, time, author, name). A revert reads the kept save like any project folder, and is carried out as the smallest changes to the project, so a track it doesn't change plays on untouched; nothing on disk changes until the next save, which keeps the version it replaces as usual. MCP's `save` is described as for when the user asks, and its result names the save it kept.
  - **Text-only copies stay enough once audio arrives** (Milestone 7): audio files are never modified (§4.3). Recording and importing write new files, and every edit (offsets, fades, gain, comps) lives in `project.json`. So a save's text files say everything about the project, and the audio they name is still in the folder. An audio file no clip uses any more goes to `unused/` like a clip file, never deleted, and reverting to a save that names it moves it back.
- Loading, `--check`, `--watch` and the outside-change check all ignore `.hocket/` and `unused/`. The repository's example projects keep `.hocket/` out of git.
- **The recovery copy:** about 3 s after changes stop (and at least every 30 s while they keep coming), Hocket writes the whole project outside its folder, so a crash loses nothing: to the project's own place in `%LOCALAPPDATA%\Hocket\Recovery` on Windows, named for its folder and a hash of its full path (`my song 3f2a9c81d4e6b7a0`), as a project folder of its own that loads like any other. Each copy also names the project's ID, 32 hex digits kept in the project's own `.hocket/id` (made with its first copy; kept saves don't copy it), so after the folder is moved or renamed its copy is still found, and its place taken over for the new path. Not when the old path still holds a project with that ID: then the folder was copied, and the copy is the original's (Lee, 2026-11-22). A copy found that way may still be another folder's (one copied, whose original was then renamed), so every message about it names the folder it was written for, and plain `--play`, stopping to ask, says the folder may have been copied rather than moved (Lee, 2026-11-24). It's there only while the session holds changes that aren't on disk: it's removed after a save, after undo back to the saved state, and at a clean exit with nothing unsaved; a clean exit with unsaved changes keeps it and says where. It holds the project, not its history.
  - **A crash or power loss part-way through writing it leaves the copy before it whole and usable, never a half-written one** (Lee, 2026-11-21). Each copy is written whole into a new numbered folder (`writing 8`), every file on the disk before the next step, then renamed into place (`8`) in one step, and flushed, which on NTFS writes the rename to the disk (Windows promises no more for a rename, so a power cut at that very moment may undo it, leaving the copy before it, whole); only then is the one before (`7`) taken away, renamed first, so a removal cut short leaves it whole or gone. Reading takes the newest whole copy, and the next write clears away what a crash left part-way.
  - **A copy an earlier session left** holds changes it never saved, and plain `--play` won't play the project then (Lee, 2026-11-21): it plays nothing, and says when the copy was written, whether it's newer than the project's files, what it changes, and the two ways on. `--play --recover` plays it: its changes come in as one history entry replacing the project (the command a revert uses), unsaved, which undo takes back. If they leave the project without being saved (that entry undone, say), the copy is filed with the kept saves as a discarded recovery before it's removed, so an undo never loses recovered work (Lee, 2026-11-21). `--play --discard-recovery` plays the files as saved, first filing the copy with the kept saves, labelled as a discarded recovery, so revert can still bring it back; nothing is deleted. A copy that holds nothing the files don't (a crash between a save and the copy's removal leaves one) is removed without asking. The GUI will ask the same question when it opens such a project.
  - **When the project's files don't load** (a hand edit broke `project.json`, say), plain `--play` prints their problems and plays nothing, and says where the recovery copy and the kept saves are, and how to reach them: the newest kept saves by name and folder, which restore by hand. `--play --recover` still works: the session starts as the copy's project, unsaved, with no history entry, since there's no loadable project for an undo to go back to; the copy stays until a save, which keeps the files on disk first, as they are (Lee, 2026-11-22). `--play --discard-recovery` files the copy with the kept saves, and plays nothing.
- **Outside changes:** saving never overwrites a file that changed outside Hocket since Hocket last read or wrote it (§6.1), was deleted outside it, or is at a new clip's path but was never in the project (put there by hand): it writes nothing, and names each file and what happened to it. Saving with `overwrite` writes anyway, after keeping the version on disk as a kept save, labelled "changed outside Hocket" at its latest change's time. Hocket notes each project file's size and modified time whenever it reads it (just before) or writes it (the new file's, before it's swapped in, so a change made the moment after still shows). A file whose text the save wouldn't change never stops it, and `.hocket/` and `unused/` are never checked.
  - `--watch` covers `project.json` as well: a saved change becomes one command that replaces the project, checked and undoable like any other (the command a revert uses). When it replaces changes to the project that weren't saved (a track an AI client added, say), it warns as a clip file's save does (§6.1), naming the latest and the entry to undo. It's read with each clip file as the session has it, unsaved changes included, unless that file also changed on disk or is new, when it's read from disk. A change `--watch` can't take in (a `project.json` that doesn't read, say) is reported once, and the session carries on as it was; the next save won't overwrite it unasked. A `project.json` it couldn't take in is read again once it, or a clip file it names, is saved: one naming a new clip file saved after it goes in then.

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
| Position | `bar.beat[.sixteenth][+ticks]` | 1-based, relative to the clip start. Beats are the time signature's, and the sixteenth counts within the beat (§4.4). `+ticks` allows micro-timing (960 ticks per quarter note). |
| Pitch | Note name + octave (`C2`, `F#4`, `Bb3`) | Uppercase letter, optional `#` or `b`, octave. `C4` = MIDI 60 (middle C); range `C-1` to `G9`. No key-signature inference — the written name is the pitch, and its spelling (`Eb2` vs `D#2`) is kept. On a drums track, lowercase names are its drum map's (§5.2). |
| Length | Fraction of a whole note (`1/8`, `1/16`, `3/8`), triplets (`1/8t`), or ticks (`250tk`) | |
| Velocity | `v1`–`v127` | Required on every note line. |
| Controllers | `ccN`, `pb`, `at` (channel aftertouch) | Values or shapes, same shape syntax as automation. Per-note (polyphonic) aftertouch: TBD. |
| Comments | `#` to end of line | `#` starts a comment at the start of a line or after whitespace, so the `#` in `F#4` is not one. Preserved on canonical rewrite. |

### 5.1 Decided details
- **Timing resolution:** 960 ticks per quarter note, whatever the time signature. `+ticks` offsets are for human feel and micro-timing.
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
A drums track (`"type": "drums"`) has a **drum map** in `project.json` that maps names to MIDI note numbers (§4.4). Clips on that track use the names, in canonical form like this:
```
1.1      kick  1/16  v110
1.1      hat-closed  1/16  v80
1.2      snare 1/16  v100
1.2.3    snare 1/16  v28  # ghost note
```
- **The map lives on the track**, in `project.json`, so a project reads and validates without any kit present. A kit's manifest maps notes to samples. The map is the project's own: a kit only adds to it, when it's assigned.
- **Assigning a kit** keeps every entry already in the track's map, and adds the kit's names for notes and names the map doesn't use yet, so nothing renamed or added is lost. A name isn't added either when a file the track shares with another track (below) would then name its notes differently on the two. The result says which of the kit's pieces were added, and which weren't and why (the note already has another name, which plays the piece; the name is on another note; or a shared file), with the note name that plays a piece left unnamed. Assigning the kit a track already has merges again, so pieces added to the kit since get their names; it changes nothing only when nothing would be added. A kit that isn't installed can't be assigned (§8.4).
- **What the kit can't play:** a map usually names more than its kit has (the standard list names 47 pieces). The overview and `write_clip`'s result say which of the map's names, and which notes in a clip, the track's kit can't play.
- **Standard names:** one list of piece names on the General MIDI percussion notes (below). A drum track with no kit gets this list as its map. Bundled kits (§8.4) use these names on these notes, and may add extra pieces on other notes.
- **Naming rules:** lowercase letters and digits in words joined by single hyphens, starting with a letter, at most 32 characters. One note per name and one name per note.
  - A name can't read as anything else on a note line: not a note name in lowercase (`c2`, `eb3`), a controller (`pb`, `at`, or `cc` and a number), a velocity (`v` and a number), or a MIDI channel (`ch` and a number).
- **Descriptive names beat numbers** where a kit allows it: `crash-left` / `crash-right`, `crash-18`, `china`, `splash`. The standard list numbers General MIDI's duplicates (`crash` / `crash2`) because General MIDI doesn't say what they are.
- **Articulations are their own entries:** `hat-closed`, `hat-open`, `hat-pedal`, `ride`, `ride-bell`, `snare-rim`, `sidestick`.
- **Writing:** a note the map names is always written by its name, however it was written: `D2` becomes `snare`, as `Cb4` becomes `B3` (§5.4). Its spelling isn't kept: if the map stops naming it, a black key is written sharp.
- **Fallback:** a note with no map entry is written as its note name (`A#5`), so nothing is ever lost or unreadable. `write_clip`'s result lists such notes on a drums track, since they only sound if the kit has a piece there.
- A drum name on a track with no drum map is an error that says so, and so is a name the map doesn't have, with the map's names.
- **A file linked into several tracks** (§4.4) must mean the same notes on each, and be written the same way for each: every note in the file has the same name in each track's map, or no name in any. A track with no drum map names nothing. It's an error otherwise, and the message names the notes, the names, and the tracks. This may later be relaxed, so that each track reads the file with its own map.
- **On the drum sampler, a note's length changes nothing.** Its pieces are one-shots: each hit plays its sample to the end, unless a choke group cuts it, so a stop or a seek leaves hits ringing, and so does a clip that ends or is rewritten under one. Lengths are still kept, for MIDI export and other instruments; `1/16` is the usual one to write.
- **Choke groups** belong to the kit (its manifest, §8.4), not the notation. A hit cuts every sounding hit in its group, its own piece's included, so an open hat struck again restarts, and a closed hat cuts an open one. The cut is a 10 ms fade from the cutting hit's sample. Pieces in no group ring over each other, like cymbals; a kick that should cut itself, like an 808's, goes in a group of its own.

**The standard names**, on General MIDI's notes. Note names are Hocket's, where C4 is 60, so the kick is C2; many drum charts call it C1, counting middle C as C3.

| Note | Name | Note | Name | Note | Name |
|---|---|---|---|---|---|
| 35 (B1) | `kick2` | 51 (D#3) | `ride` | 67 (G4) | `agogo-high` |
| 36 (C2) | `kick` | 52 (E3) | `china` | 68 (G#4) | `agogo-low` |
| 37 (C#2) | `sidestick` | 53 (F3) | `ride-bell` | 69 (A4) | `cabasa` |
| 38 (D2) | `snare` | 54 (F#3) | `tambourine` | 70 (A#4) | `maracas` |
| 39 (D#2) | `clap` | 55 (G3) | `splash` | 71 (B4) | `whistle-short` |
| 40 (E2) | `snare2` | 56 (G#3) | `cowbell` | 72 (C5) | `whistle-long` |
| 41 (F2) | `tom-floor-low` | 57 (A3) | `crash2` | 73 (C#5) | `guiro-short` |
| 42 (F#2) | `hat-closed` | 58 (A#3) | `vibraslap` | 74 (D5) | `guiro-long` |
| 43 (G2) | `tom-floor-high` | 59 (B3) | `ride2` | 75 (D#5) | `claves` |
| 44 (G#2) | `hat-pedal` | 60 (C4) | `bongo-high` | 76 (E5) | `woodblock-high` |
| 45 (A2) | `tom-low` | 61 (C#4) | `bongo-low` | 77 (F5) | `woodblock-low` |
| 46 (A#2) | `hat-open` | 62 (D4) | `conga-high-mute` | 78 (F#5) | `cuica-mute` |
| 47 (B2) | `tom-mid-low` | 63 (D#4) | `conga-high-open` | 79 (G5) | `cuica-open` |
| 48 (C3) | `tom-mid-high` | 64 (E4) | `conga-low` | 80 (G#5) | `triangle-mute` |
| 49 (C#3) | `crash` | 65 (F4) | `timbale-high` | 81 (A5) | `triangle-open` |
| 50 (D3) | `tom-high` | 66 (F#4) | `timbale-low` |  |  |

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
- **`@length`:** always written, in bars of the song's time signature (`@length 1bar`, `@length 4bars`). A file without it gets the smallest whole number of bars that holds every note's start (at least one).
- **Comments:**
  - Comments before the first directive or note line are the file header and stay at the top.
  - Any other full-line comment belongs to the line after it and moves with that line. A trailing comment belongs to its own line.
  - Text is kept exactly, minus trailing whitespace. Full-line comments lose indentation; trailing comments go two spaces after the line.
  - When lines merge into a chord, all their comments are kept: full-line comments in their original order, trailing comments joined with `; `.

Validity rules: a note may not start at or after the clip's end, but may extend past it (cut off at playback). Two notes with the same position and pitch are an error. A clip is at most 999 bars long, whether `@length` says so or the length comes from the notes, so a typo in a bar number can't make a clip, and a loop, thousands of bars long.

---

## 6. AI integration

### 6.1 Source of truth
| DAW state | Source of truth | How AI edits |
|---|---|---|
| **Open** | The running DAW | Through the live API (MCP). |
| **Closed** | The project folder | Directly on files (e.g. Claude Code scaffolds a song from scratch); DAW loads it on open. |

If the project folder is modified externally while the DAW is open, the DAW **detects it and asks** the user whether to reload or merge. It never silently overwrites either side.

The headless host has an opt-in exception for development, `--play --watch`: each file of the project saved outside Hocket is taken in without asking, as the person's command, so it's validated and undoable like any other edit: a clip file as a `write_clip`, `project.json` as one command replacing the project (§4.5). It compares the files with what Hocket last read or wrote, so Hocket's own saves are never taken for outside changes. A change made through the API since the last save isn't in the file, so a file's save outside Hocket replaces it; when it does, `--watch` warns, naming the latest change it replaced and the entry to undo to get it back. Hocket's own saves never overwrite an outside change unasked (§4.5).

### 6.2 The API speaks the text format
Reading a clip returns `.notes` text; writing a clip accepts `.notes` text or a patch. The model's "language" is identical whether working live or on saved files.
- `read_clip` returns the stored text in canonical form, verbatim, in a content block of its own; the clip's details (ID, track, file, position, length) come in the block before it. A model can edit the text and send it straight back.
- `write_clip`'s result names the new history entry and says whether the text was stored exactly as sent. When canonical form differs, it says briefly how (lines rewritten, with examples; lines sorted; spacing; blank lines), so the model knows its copy is out of date. On a drums track, it also lists the notes the drum map doesn't name, and on a drum sampler's track the notes its kit has no piece for, or that none sound because the kit can't play (§5.2).
- Resizing a clip is a `write_clip` with a new `@length`. On a linked clip it resizes every clip using the file, and a longer `@length` must fit each of them. A note can't start at or after the clip's end (§5.4), so a shorter `@length` must leave out the notes past it.
- `read_clip` can return some of a clip's bars (`"5-8"`, in the clip's own bars as its positions count them): its header comments, its `@length` line, and the lines whose notes start in those bars, with their comments. That's enough for `patch_clip`, but not for `write_clip`, which takes the whole text.
- **`patch_clip`** changes some of a clip's notes without sending all of them. Each edit has `old` lines, `new` lines, or both, in `.notes` form.
  - It matches **notes, not lines**. `old`'s lines are read as notes, each of which must be in the clip at that position and pitch, with that length and velocity, and they're removed; `new`'s notes are added. So spacing doesn't matter (`read_clip`'s text is column-aligned), and a note in a chord, or in a line merged from several (§5.4), matches on its own: `old: "1.1 hat-closed 1/16 v100"` takes the hat out of `1.1 [kick hat-closed] 1/16 v100`. Pitches compare by number, so `Eb2` matches `D#2`, and a drum name matches its note.
  - **Comments:** a comment in `old` must be the stored line's; a line in `old` without one matches whatever comment the stored line has. A comment stays with its line while any of that line's notes remain, and goes with the last of them. Comments in `new` go with their lines.
  - Edits apply in order, each to the clip the previous one left, **all or none**; the clip is then checked and stored as `write_clip`'s is, and the result is `write_clip`'s, with what each edit did. `patch_clip` changes notes only: `@length` changes through `write_clip`.
  - **When a note isn't found**, the error shows the clip's lines in that bar now. **When it's found but its length, velocity or comment differs**, the error says which, and shows the stored line. A note in `new` that's already in the clip (same position and pitch) is an error that says to put it in `old` too.

### 6.3 Transport
- The DAW exposes a **localhost WebSocket API**, on 127.0.0.1 only. (Chosen over Windows named pipes: easier to debug and not platform-specific.)
- A small **MCP server** bridges MCP clients (Claude Desktop, Claude Code, local-model agents) to that API.
  - The bridge (`hocket-mcp`, C++, stdio transport) is a thin adapter. It answers MCP's `initialize` and `tools/list` itself, and forwards each `tools/call` to the WebSocket API, which it finds through the token file. Connecting Claude Code: `docs/connecting-claude-code.md`.
  - **There is one list of tools**, defined next to the command bus (`hocket_api`). The DAW and the bridge compile in the same definitions, so a client sees Hocket's tools before Hocket starts, and `hello`'s build check makes sure the DAW it reaches has the same list.
  - It connects on the first tool call, and again after the connection closes. It re-reads the token file on every call, so a restarted DAW (new port and token) is found, and it keeps its session ID throughout. After a restart, the next result tells the model that the history has started over and the project may have changed.
  - A call while Hocket isn't running (no token file, one left by a crash, or a refused connection) returns an `isError` result saying how to start it: "Hocket isn't running: start it with `hocket-headless --play <project folder>`, then try again."
  - Chosen over the DAW serving MCP over HTTP itself: the bridge reads each session's token and port from the file, so a client is registered once rather than every session; stdio is the transport every local MCP client supports; and the bridge can report "Hocket isn't running" as a readable tool error and reconnect when the DAW restarts.
- **Protocol:** JSON-RPC 2.0, one request per message (no batches).
  - The first request must be `hello`, with params `{"token", "version", "fingerprint", "client", "session", "label"}`; `label` is optional. Anything else first, or a wrong token, gets an error and the connection is closed.
  - `version` is Hocket's version, and `fingerprint` is a hash of a protocol version constant and the tool definitions (`api::buildFingerprint`). The version alone stays the same through development; the fingerprint changes with the tools, and the constant is bumped for changes to `hello` or the message formats. If either differs from the DAW's own, it refuses with an error saying the two come from different builds and how to fix it (rebuild and reinstall `hocket-mcp`, then restart the MCP client), and closes the connection. The bridge returns that message from the tool call.
  - `client` is the display name: the bridge's `--name`, or else MCP `initialize`'s `clientInfo.name`, with Claude Code's `claude-code` shown as "Claude". `label` is the bridge's `--label`, or else the name of the folder it was started in (the session's folder, for Claude Code). `session` is the bridge's session ID (§3.1), 16 random bytes as hex.
  - Then `tools/list` and `tools/call`, shaped like MCP's: results are `{"content": [{"type": "text", "text": ...}], "isError": ...}`, so the bridge forwards them unchanged.
  - A tool that fails (invalid `.notes`, bad arguments, an undo that isn't the caller's) returns an `isError` result, so the model reads the message. JSON-RPC errors are only for protocol mistakes (malformed requests, unknown methods or tools), and for Hocket's own internal errors: anything that goes wrong inside the DAW while handling a request is answered with JSON-RPC's internal error (-32603), saying the request may or may not have been carried out, and Hocket keeps running.
- **Port:** 46253, or the next free one up to 46262. The one in use is in the token file.
- **Token auth:** the DAW writes a per-session token to a local file; clients must present it. Prevents arbitrary local software from controlling the DAW.
  - The token is 32 bytes from the OS's secure random source, as 64 hex digits, new each time the DAW starts.
  - The file is `%LOCALAPPDATA%\Hocket\api.json` on Windows: `{"pid", "port", "program", "token"}`. It's written atomically when the API starts and deleted on exit.
  - One DAW serves the API at a time. If the file names a running Hocket (same process ID and program name, so a reused ID doesn't count), a second one refuses to start. A file left behind by a crash is replaced. So that two starting together can't both find none, each first takes a lock file next to the token file (`api.lock`), held until it exits; the OS lets go of it if it crashes.
- **No web pages.** Any web page can open a WebSocket to 127.0.0.1, and browsers always send the page's origin. The token keeps pages out anyway; on top of that, a connection whose `Origin` isn't a `ws://` or `wss://` URL is closed before it can send anything. Native clients send no `Origin`, or their own `ws://` URL (IXWebSocket's client does).

### 6.4 Tool set
Prefer a small set of expressive tools over many tiny ones. Arguments are flat: each tool does one kind of thing, except `transport`. Values are written as `project.json` writes them (`"9.1"`, `"-6dB"`, `"L20"`, `"6/8"`), objects are named by ID, never by index, and results are the command bus's own messages (`#12 set trk-bass's volume to -6dB`). Tool descriptions, results and errors are written for an LLM reader.

Milestone 1's set (agreed with Lee on 2026-11-21), 22 tools:

| Tool | Arguments | What it does |
|---|---|---|
| `get_project_overview` | none | The tempo, time signature and song length; the transport and output lines, as `transport` gives them; every track with its mix, devices, drum map and clips (IDs, positions, lengths, note counts, links); the drum kits in use, with the names each plays, and those installed; the latest history entry; any open transaction; whether anything is unsaved. |
| `read_clip` | `clip_id`, `bars`? | A clip's `.notes` text, whole or some of its bars (§6.2). |
| `write_clip` | `clip_id`, `notes` | Replaces a clip's notes; a new `@length` resizes it (§6.2). Its description has the `.notes` reference and the standard drum map. |
| `patch_clip` | `clip_id`, `edits`: [{`old`?, `new`?}] | Removes and adds notes (§6.2). |
| `add_clip` | `track_id`, `start`, one of `bars` (empty), `notes`, `linked_to`, `copy_of`; `id`?, `file`? | A new clip. Hocket makes its ID and file name unless given. |
| `move_clip` | `clip_id`, `start`, `track_id`? | To a new start, and another track if given. |
| `remove_clip` | `clip_id` | |
| `make_clip_unique` | `clip_id`, `file`? | Gives a linked clip a file of its own. |
| `add_track` | `type`, `name`, `instrument`?, `kit`?, `before`?, `after`?, `id`? | A drums track gets the standard drum map. |
| `set_track` | `track_id`, any of `name`, `before`, `after`, `volume`, `pan`, `mute`, `solo`, `instrument` (`"none"` too), `kit`, `drum_map` | What one call changes is one history entry. |
| `remove_track` | `track_id` | With its clips. |
| `set_song` | `tempo`?, `time_signature`? | Both at once are one history entry. |
| `transport` | `loop_region`?, `loop`?, `seek`?, `action`? | Applied in that order; with no arguments, the transport's state (below). |
| `undo` / `redo` | `force`? | `undo` refuses when the newest entry isn't the caller's, and `redo` when someone else did the undo it would take back, unless `force` is passed (§3.1). |
| `history` | `limit`?, `author`? | The latest entries, newest first. `author` is `"you"`, `"others"`, or a name as history shows it. |
| `begin_transaction` / `commit` / `rollback` | `name` (begin) | One named undo step (§3.1). |
| `save` | `name`?, `overwrite`? | Saves the project (§4.5). Its description says to save when the user asks, and to pass `overwrite` only when they say to save over files changed outside Hocket. |
| `list_saves` | `limit`? | The kept saves, newest first, and whether anything is unsaved. |
| `revert` | `save` | Brings back a kept save, as one history entry. Its description says to revert only when the user asks. |

- **The overview's drum maps** are shown as their differences from the standard map (§5.2), which `write_clip`'s description lists once: `drum map: standard`, or `standard, rim=37, -cabasa`; a map whose whole list is shorter is shown whole, as `empty, kick=36, snare=38`. That form is also what `set_track`'s `drum_map` takes: `"rim=37"` names note 37 rim (its old name goes, since a note has one name, and rim leaves any other note), `"-cabasa"` removes a name, and a first item `standard` or `empty` starts from that map rather than the track's. Notes never change, only the names clips use for them.
- **The overview shows a clip's file** only when it isn't the one the clip's ID implies (`clp-verse` → `clips/verse.notes`), or the clip is linked; a tool description says so.
- **The transport's state:** what play plays (the loop region or the whole song), on a loop or once, where play starts, whether it's playing and where the playhead is; and the **output line**: the peak level and the count of samples over full scale, which restart at each transport play, with how much has played since: the time, and the complete passes of what plays when looping ("Output since play started: 42 s, 2 complete passes; peak +0.4 dBFS, 312 samples over full scale, so the mix clips"). It needs `--play`'s master meter (§9), whose exit report still covers the whole run.
- **A change that moves the playhead or stops playback says so** in its result: one that ends the song before the playhead wraps it while looping (§3.3), or stops playback with loop off.
- **Annotations:** the four reading tools carry MCP's `readOnlyHint`. The others carry none until Lee decides on the rest (`destructiveHint` especially, whose effect on Claude Code's approval prompts was looked into on 2026-11-21).

Later milestones add `read_automation` / `write_automation` (Milestone 6), routing and device tools with `set_params` (Milestone 5), and `render_section` / `analyze` (Milestone 8, §7).

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

**Native device types and parameters are Hocket's, whatever runs them.** A native device's type and parameter names (`hocket:synth`, `cutoff`) are Hocket's from the start, since they're part of the file format and the API.
- Behind a Hocket name, one of Tracktion's devices may stand in until Hocket's own replaces it, wherever it passes the real-time stress tests (§3.2). The replacement changes no project files.
- Hocket's own synth comes first. It replaces 4OSC, which allocates on the audio thread and renders slowly.
- Hocket's synth leaves headroom by default: a full-velocity chord on one track stays under full scale with the track at 0 dB. (On 4OSC, a five-note chord at v127 peaks at +11 to +13.5 dBFS, by where its oscillators' phases fall.) Until then, 4OSC is trimmed by 12 dB, so one note at v127 peaks at about −12 dBFS, as the bundled kits' kicks do (Lee, 2026-11-20); that chord then peaks at about −1.4 to +1.5 dBFS.
- So far there's `hocket:synth`, which Tracktion's 4OSC plays until Milestone 4, and `hocket:drum-sampler`, with a kit (§8.4).
- The drum sampler is Hocket's own from the start. Tracktion's `SamplerPlugin` takes a lock the message thread shares, and allocates for every note-on, on the audio thread.
  - Velocity sets a hit's level: its gain is the piece's level × (velocity / 127)², General MIDI's default curve (v100 is −4.2 dB, v64 −11.9 dB, v28 −26 dB). It adds no gain of its own: at v127, a piece at a level of 0 dB plays its sample exactly.
  - Samples are converted to the device's sample rate once, when a kit loads or the rate changes, band-limited so nothing folds back; at equal rates they aren't touched. The conversion is flat to 20 kHz within 0.1 dB, with aliasing at least 90 dB down: libsamplerate's best sinc converter, which Tracktion bundles, meets it; not JUCE's `WindowedSincInterpolator`, which doesn't filter when converting down, and not r8brain (Lee, 2026-11-17).
  - **Kits load on a loader thread**, never the message thread, where a second's stall would freeze the window (Lee, 2026-11-17). Until a new kit is ready, the track plays the kit it had (silence if none), and a result for a kit since replaced is dropped. `--play`'s start, and offline renders, wait for the kits.
  - **Voices:** a fixed pool of 64. When it's nearly full, the oldest hit still at full level fades out (10 ms) to make room, so no hit is ever dropped, or cut without a fade. Tracktion's panic or reset fades every hit; hits start on their exact sample.
  - **Kits leave headroom:** the heaviest realistic hit, kick, kick2, snare, clap, open hat and crash struck together at v127 (those of them a kit has), on a track at 0 dB and C, peaks at −1 dBFS or under, at 44.1 and 48 kHz. Every piece at once is no rule: the output meter covers that (§9). Bundled kits are tested against it.

### 8.2 VST3 hosting requirements (Phase 2)
- Expose parameters with names, normalized values, and display strings.
- **Parameter mapping files** per plugin to rename cryptic parameters (`Macro 3` → `Filter Drive`), built up over time and shareable.
- Save/load/swap plugin state and presets.
- **Out-of-process sandboxing** so a crashing plugin doesn't take down the DAW.
- Latency reporting and compensation.
- **The song's time signature and bar position.** Plugins read the tempo, time signature and position in bars from the host (an arpeggiator, a synced delay or LFO). The Tracktion Edit's own time signature is 4/4 for now, whatever the song's (§4.4), so hosting needs the song's to reach plugins first.

### 8.3 Test plugin set (all free/legal)
Surge XT, Vital (free tier), Dexed, Airwindows, Valhalla Supermassive, TDR Nova, MeldaProduction free bundle, Cardinal, Steinberg VST3 SDK example plugins. MiniMeters (already owned by Lee) as a commercial test case. Commercial plugins (incl. copy protection like iLok) tested via official demos. Serum 2 is the one planned purchase, once VST hosting exists.

### 8.4 Bundled drum kits
Hocket comes with a few drum kits in different styles: acoustic, electronic (808-style), and one suited to breakbeat and jungle.
- **Licensing:** only samples under an explicit permissive license (CC0 or similar), and nothing taken from copyrighted recordings. Every source and license is listed in DEPENDENCIES.md.
- **Size:** compact, short 16-bit one-shots, so the repository doesn't bloat. DEPENDENCIES.md notes the total size.
- **A kit is part of Hocket, like a preset.** A project names its kit (the drum sampler's `kit`, §4.4), and nothing is copied into the project folder. A released kit's samples never change; a changed kit gets a new name. A bundled kit counts as released only once Lee says it's final: until then it's refined by ear in place.
- **Levels:** each bundled kit's samples peak at −1 dBFS on their own, and the balance between pieces is `kit.json`'s `level`. A bundled kit's kick at v127, on a track at 0 dB and C, peaks at −12 dBFS, and its other pieces are balanced against it by ear. Every bundled kit's kick matches, so changing kits keeps the level. Kits also keep to the headroom rule (§8.1); bundled kits are tested against both.
- **The electronic kit is synthesized** by a generator kept in the repository (`tools/kits/`), with named parameters for each piece, so it's changed by changing a parameter and generating it again. Its samples are 44.1 kHz, 16-bit and mono, and released as CC0. It has 23 pieces, the standard names on the standard notes, with all six toms there so a General MIDI tom fill never lands on silence; its choke groups are the three hats together, and `kick` and `kick2` each alone, so each cuts itself but the two can be layered. No piece's polarity is flipped to win headroom (Lee, 2026-11-18 to 20).
- **A missing kit:** a project whose kit isn't installed still loads. Hocket says the kit is missing, and the track is silent.
- **Names:** a kit's manifest maps notes to samples, and names any pieces beyond the standard ones. Bundled kits use the standard names on the standard notes (§5.2), and may add extra pieces on other notes. Assigning a kit to a track adds its names to the track's drum map, keeping what's there (§5.2). A kit that isn't installed can't be assigned: the error lists the kits that are, with "did you mean", and says where Hocket looked (or where kits go, when none are installed) (Lee, 2026-11-17).
- **The manifest**, `kit.json` in the kit's folder, beside its samples:
  ```json
  {
    "format": "hocket-kit",
    "formatVersion": 0,
    "description": "Electronic, 808-style",
    "pieces": [
      { "name": "kick", "note": 36, "file": "kick.wav", "level": "0dB" },
      { "name": "hat-closed", "note": 42, "file": "hat-closed.wav", "level": "-4dB" },
      { "name": "hat-open", "note": 46, "file": "hat-open.wav", "level": "-4dB" }
    ],
    "chokeGroups": [["hat-closed", "hat-open"]]
  }
  ```
  - Strict, like `project.json`: every key shown is required, and unknown keys are errors. The kit's name is its folder's.
  - Piece names follow the drum map's naming rules (§5.2); one note per piece and one piece per note. `level` is in dB, like a track's volume. Choke groups name pieces, each in at most one group (§5.2).
  - Samples are WAV files, 16- or 24-bit, or 32-bit float, mono or stereo (a mono one plays the same in both channels), at 8 to 384 kHz, at most 10 s each; bundled kits' are 16-bit. The usual layouts of real sample packs read: the extensible format header, and other chunks (LIST, smpl, acid, bext and the like) before and after the audio. A file whose header Hocket can't make sense of is left to the decoder to accept or refuse. One sample per piece; velocity layers and round robins may come with the full sampler (Milestone 4), as additions to the format.
  - A 32-bit float sample can peak above full scale; the overview says which pieces do, and by how much.
  - A kit that's installed but broken (a bad manifest, an unreadable sample) counts as missing, and Hocket says why. So does one that can't be loaded (its samples converted to the device's rate, say): nothing that goes wrong while a kit loads stops Hocket.
- **Where kits are:** bundled kits in `kits/` next to Hocket's programs (from `assets/kits/` in the repository), and the user's own in `Documents\Hocket\Kits\` on Windows (the Documents folder as Windows reports it, so it follows OneDrive). Hocket doesn't create that folder.
  - **The user's kits are named with `user/`** in a project (`"kit": "user/dusty-909"` for `Documents\Hocket\Kits\dusty-909\`), so they never clash with a bundled kit, even one a later release adds. The folder's own name has no prefix, and follows the same naming rules as a bundled kit's.
  - Kits are listed at start, and again whenever a name isn't found, so a kit dropped in while Hocket runs can be assigned without a restart. A track whose kit wasn't there, or was broken, when it was set up plays it once it's installed or fixed: Hocket looks for it again every half second, and loads it once its files have stopped changing (bug sweep, area 2).
  - **The user's kits that tracks use are watched**, every half second and when play starts: a saved change to `kit.json` or a sample is heard on the next hit, converting only the samples that changed, even a change saved after Hocket first read the kit but before a track used it. A file is read once it has stopped changing. A change that can't be read (an error in `kit.json`, an unreadable sample) leaves the track playing the last version that loaded, and Hocket says what's wrong. Bundled kits never change, so they aren't watched.
- The overview says which kit each drums track uses, where it came from (bundled, or the user's folder), and which of the track's drum-map names it can play, with a count of the rest.

---

## 9. Platform notes
- Windows first: ASIO and WASAPI via JUCE. (ASIO SDK is available under a GPLv3 option; VST3 SDK is MIT since 3.8.)
- Audio drivers vary more than their names suggest. Many people's only ASIO is a wrapper (ASIO4ALL, FL Studio ASIO), and on Milestone 0's test machine ASIO4ALL buzzed at a 512-sample buffer while FL Studio ASIO played cleanly at the same size. It was the buffer size, not the sample rate: ASIO4ALL still buzzed at the rate Windows was using, and was clean at 1024 samples. ASIO4ALL talks to the device directly, so with that USB DAC it most likely needed more room than a driver that goes through Windows' audio engine. Not one driver tried there reported xruns. So:
  - v1's audio settings let the person choose the sample rate and buffer size, rather than taking whatever the driver offers;
  - Hocket counts late audio callbacks itself, since the driver's xrun count often isn't there.
- Some drivers (ASIO, Windows Audio in exclusive mode) bypass the Windows volume slider, so playback can be far louder than other audio at the same setting. Until the master bus (Milestone 5), `hocket-headless --play` has a master level of its own: 0 dB by default, the project as it's mixed, set with `--master-volume <dB>` (written like track volumes), and not saved in the project. (It was −15 dB until the bundled electronic kit, its kick at −12 dBFS, was barely audible at full volume: Lee, 2026-11-20.) It applies to what's sent to the device, so offline renders don't include it. It prints the level at start, and at exit the peak output level next to the xrun count, with a warning if the output clipped that suggests a `--master-volume` to keep it under full scale. Over a driver that bypasses the Windows volume, `--play` warns to turn the device's own volume down first.
- Keep platform-specific code isolated so Linux (JACK/PipeWire) and macOS (CoreAudio, AU) ports are additive.
- Unicode paths: all text is UTF-8 inside Hocket. On Windows, text from the OS (paths, command line) is converted from UTF-16 at the platform boundary, never through the ANSI code page, so folders with any name work.
- Long paths: Windows limits a path to 260 characters unless its long-path setting (LongPathsEnabled) is on and the program says in its manifest that it can take longer ones. Hocket's programs that read or write project folders say so, so a project in a deep folder works where the setting is on. Where it's off, a kept save's paths (`.hocket/saves/<name>/clips/...`) are the first to pass 260: the save is refused, writing nothing, and says to move the project or turn the setting on.

---

## 10. Scope and roadmap

### 10.0 Milestone 0 — headless proof (before any GUI work)
**Done, 2026-11-06.** Checked over ASIO on the test laptop: a fresh clone builds and its tests pass; the demo loops; a model's rewrite from Claude Code is heard on the next pass with no glitch, undo restores it, and history names who made each change; 200 rewrites at 20 a second play with no late block and no audible dropout. What it taught is folded into §3.2, §3.3, §9 and §11.

A command-line program that:
1. Loads a project folder (`project.json` + one `.notes` clip),
2. Plays the clip through a native synth over ASIO,
3. Lets an MCP client rewrite that clip **while it plays**, with the change landing as an undoable command.

Purpose: prove the riskiest architecture pieces work together early — text format, command bus, Tracktion sync, and real-time safety.

The native synth for Milestone 0 is Tracktion's built-in 4OSC, as a stand-in; Hocket's own synth comes in v1 (§10.1).

Milestone 0 `project.json` subset: §4.4. Milestone 0 `.notes` subset: note lines, chords, comments and `@length`, in 4/4 only. Controller lines (`cc`, `pb`, `at`), `@swing`, `@ch`, per-note `chN` and drum names are rejected with an error saying they aren't supported yet.

### 10.1 v1 — "Lee and Claude can make a complete song in it, start to finish, with native instruments"
- Engine on Tracktion; ASIO/WASAPI; transport and looping; one tempo and time signature per song
- MIDI tracks, piano roll, `.notes` format, MIDI import/export
- **MIDI recording** from a keyboard (Lee's Yamaha piano, on the desktop)
- Native devices (§8.1): Hocket's own synth, a sampler/drum sampler with bundled kits (§8.4), EQ, compressor, reverb, delay, filter/saturator
- Audio tracks: audio import and **recording** (arm a track, record takes — vocals are a v1 requirement)
- Mixer: volume, pan, sends, master bus; basic automation
- Command bus, undo with author tags, transactions
- Project format with canonical writer and round-trip tests; published JSON Schema for `project.json` (§4.1); explicit save with a recovery copy (§4.5)
- Linked clips: several clips sharing one `.notes` file (§4.4)
- MCP server with the core tool set (§6.4)
- Analysis toolkit core: loudness, spectrum, masking, before/after comparison
- Export to WAV/FLAC, including **seamless loop export** (for game music) and per-track stem export
- GUI: arrangement view, piano roll, mixer — functional rather than polished, but built on a **theming/style token system from day one** so Phase 2 polish is restyling, not rewriting

Not in v1: an installer, and a public release.

**Milestones** (agreed 2026-11-06). Nine milestones, each ending in something Lee can hear or use, checked the way Milestone 0 was. Milestone 1 grows the model without a GUI, and Milestone 2 puts a window on it. From then on, each feature arrives through every layer at once: the command, the Edit sync with its stress test, the MCP tool, and the GUI. Each milestone's steps are planned in PROGRESS.md when it starts.
1. **A song, not a loop** (no GUI yet). Commands for the song's structure: tracks, clips, linked clips, tempo, time signature, each track's volume, pan, mute and solo, seeking and the loop region. Each is synced to the Edit and stress-tested. Also:
   - Hocket's drum sampler, with the first kit, so the first multi-track song has drums.
   - Saving (§4.5).
   - MCP tools for all of it.
   - Milestone 0's loose ends, and choosing the sample rate and buffer size (§9).
2. **The window.** `hocket.exe`, a JUCE app serving the same API, so Claude connects to it as to `--play`; `hocket-headless` stays, for tests and the acceptance run.
   - Style tokens from the first component.
   - A transport bar, and the arrangement view: create, move, resize, duplicate and link clips, or make a linked clip unique again.
   - A history panel showing who made each change; undo and redo.
   - Open and save, the prompt for changes made outside Hocket (§6.1), and the recovery copy.
   - Audio settings: device, sample rate, buffer size.
   - Claude's edits appear as they land.
3. **Piano roll and MIDI.** A piano roll with a velocity lane; a drag is one undo step (§3.1).
   - Playing and **recording** from a MIDI keyboard.
   - Device changes during playback (§11), which a piano plugged in mid-session makes urgent.
   - `@swing` and `@ch`; MIDI import and export.
   - Tested on the desktop with Lee's Yamaha piano, so the desktop needs the toolchain by then.
4. **Instruments.** Hocket's synth, which replaces 4OSC, and the full sampler/drum sampler with all three kits (§8.4).
   - `add_device`, `remove_device`, and `set_params` in real units.
   - Device panels, drawn from each device's parameter definitions.
5. **Effects and mixer.** EQ, compressor, reverb, delay, filter/saturator (Tracktion stand-ins where they pass, §8.1).
   - The saturator has a hard-clip mode, with its drive in dB, on a track or on the master bus: deliberate clipping (a phonk kick, a clipped master) is a sound people want.
   - Return buses, sends, the master bus, `set_routing`.
   - The mixer view, with meters.
6. **Automation.** The `.auto` parser and writer with the full set of tests (§4.2), `read_automation` and `write_automation`, automation lanes, and controller lines in `.notes`.
7. **Audio and recording.** Audio import; audio clips with offset, fades and gain (§4.3); waveforms.
   - Arming a track and recording takes, each entering the history as the person's command (§3.3).
   - Monitoring, and latency compensation.
   - Tested on the desktop with the Scarlett 2i2.
8. **Render, export, analysis.**
   - `render_section`, and export to WAV/FLAC, with seamless loops and stems.
   - The analysis core (§7) and `analyze`.
   - Tracktion's renderer stops every transport, so rendering while someone listens needs a second Edit. Renders wait for the drum kits to load, as `--play`'s start does (§8.1). MCP replies over 30 s need progress notifications or a longer limit.
9. **Finishing v1.** `formatVersion` 1 and the published JSON Schema (§4.1), with migrations from then on. Lee and Claude make a complete song in it, start to finish, and fix what that turns up. A fresh-clone acceptance run.

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
- Device changes during playback: when the audio or MIDI device list changes (a MIDI keyboard plugged in, a driver's settings changed), Tracktion stops the transport. Keep playing, restart, or say so? Settled in Milestone 3 (§10.1), with MIDI input: plugging a keyboard in mid-session is a realistic case.
- Name check: confirm "Hocket" is clear on GitHub, domains, and trademarks before going public
