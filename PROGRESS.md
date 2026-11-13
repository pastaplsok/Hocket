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
| 1 | Device options and Milestone 0's loose ends | Done (2026-11-12) |
| 2 | The format grows to a song: linked clips, time signatures, track mix settings, `hocket:synth` | Done (2026-11-12) |
| 3 | Drum tracks in the format: drum maps, drum names in `.notes`, `hocket:drum-sampler` | Done (2026-11-12) |
| 4 | Commands for the song's structure, with undo and gesture coalescing | Done (2026-11-12) |
| 5 | Sync: tracks and clips added, moved and removed during playback | In progress: parts 1 and 2 of 4 done |
| 6 | Sync: mix settings, tempo, time signature; transport commands for seek and the loop region | Not started |
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
- Checked at the Magni on 2026-11-12: FL Studio ASIO opens there, and ASIO4ALL's buzz is its buffer size: it's clean at 1024 samples (Loose ends).
- Left out: text selection while `--play` runs. The GUI replaces `--play` for everyday use.

**2. The format grows to a song** — done, with tests (SPEC v0.8, §4.4 and §5)
- Lee's decisions (2026-11-12):
  - **Time signatures:** a beat is the denominator's note, and positions' third number counts sixteenths within it. Ticks are 960 per quarter note in every signature; tempo is quarter notes per minute; note lengths stay whole-note fractions. Bottom numbers 2, 4, 8 and 16; one signature per song.
  - **Volume and pan** are strings with units, read by a general value-with-unit parser that device parameters reuse in Milestone 4: `"-6dB"`, `"0dB"` (unity), `"-infdB"` (silence); pan `"C"`, `"L1"`–`"L100"`, `"R1"`–`"R100"`, with the pan law written down. Mute and solo are booleans.
- Picked here, within those (SPEC §4.4):
  - The top number of a time signature is 1–32.
  - Volume runs from −96 dB to +6 dB (Tracktion's fader top), in steps of 0.1 dB, or −inf. The writer drops trailing zeros and writes boosts with `+`.
  - Pan is a balance control: at C both channels pass unchanged, and panning lowers the far channel linearly in gain, to silence at L100/R100. Tracktion's laws all boost the near side or lower the centre, so step 6 gets this out of its linear law with a matching volume.
  - A muted track stays silent when soloed.
  - The mix keys are required and always written, like every other key.
- Done:
  - `core/units`: `units::parse`/`format` for any quantity (several units, rounding, `-inf`), `units::volume()`, and `parsePan`/`formatPan`.
  - `notes::Meter` has a numerator and denominator; `parseMeter`; positions and their errors follow the signature ("sixteenth 3 doesn't exist in 6/8: a beat there is an eighth note, so sixteenths are 1-2").
  - Linked clips: `Project::notesFiles` holds each file's notes once, keyed by file; `notesOf`, `clipsUsing`, `clipEnd (project, clip)`. The reader reads a file once and reports its errors once; the writer writes it once.
  - `write_clip` on a linked clip changes the file, so every clip using it; a longer `@length` must fit all of them, and the error names the tightest. The bus's edits and change notices (`NotesChanged`) are per file, and so is `changesTo`. `--watch` watches each file once and sends a save to its first clip.
  - Tracks may have no device. Every clip ends by 1000.1 (`maxSongLengthInBars`), in the reader and in `write_clip`.
  - `"hocket:synth"`, played by 4OSC. An old `"tracktion:4osc"` gets an error saying it's now `"hocket:synth"`. The demo is in the new format.
  - `EditSync` builds any signature: ticks become Tracktion beats by the signature's ticks per beat (Tracktion's default `lengthOfOneBeatDependsOnTimeSignature()` makes its beats the denominator's note and its BPM quarter notes per minute, `tracktion_Tempo.h`). Render tests confirm notes sound on time in 6/8, 7/8, 3/2, 5/16 and 3/4.
  - The overview shows each track's mix, what a beat is outside x/4, and which clips are linked; `read_clip` and `write_clip` name linked clips. `write_clip`'s description has the new position rules, so `hocket-mcp` was reinstalled.
  - Parser messages no longer say "Milestone 0": they say when the feature comes (`@swing`/`@ch` Milestone 3, controllers Milestone 6).
- Not yet played: volume, pan, mute and solo reach the Edit in step 6. Until then `--play` says so when a project has any.

**3. Drum tracks in the format** — done, with tests (SPEC v0.9, §4.4 and §5.2)
- Lee's decisions (2026-11-12):
  - The map lives on the track in `project.json`, so a project reads and validates without any kit present. One list of standard names on General MIDI's notes (SPEC §5.2), kept as written.
  - A drums track is `"type": "drums"`, with a required `"drumMap"` of names to MIDI note numbers (`"kick": 36`). One note per name and one name per note.
  - A note the map names is always written by its name (`D2` becomes `snare`). A drum name on a track with no map is an error that says so.
  - A file linked into several tracks: every note in it has the same name in each track's map, or none does.
  - Map names can't look like a lowercase note name (`c2`), a controller, `v` and a number (a velocity), or `ch` and a number (a MIDI channel).
  - On a drums track, `write_clip`'s result lists the notes the map doesn't name, and says they only sound if the kit has a piece there.
- Picked here, within those:
  - Names are lowercase letters and digits in words joined by single hyphens, starting with a letter (so the parser can tell a name from a position or a number), at most 32 characters.
  - In `project.json`, `drumMap` comes between `devices` and `clips`, by note; a drum sampler's `kit` after its `type`. Kit names are lowercase and hyphenated, like clip files, since a kit is a folder (SPEC §8.4).
  - `hocket:drum-sampler` goes on drums tracks only; `hocket:synth` on either type. A map may be empty.
  - A named note's spelling isn't kept: the model spells it sharp on a black key, so the parsed clip is the same however it was written.
  - A track with no drum map names nothing, so a file shared between a `"midi"` track and a drums track is fine only while the map names none of its notes.
- Done:
  - `notes::DrumMap` (`core/notes/DrumMap.h`): the names both ways, `drumNameProblem` for the naming rules, `nearestNames` for "did you mean", and `standardDrumMap()`, SPEC's table. `core/Suggestions.h` has `editDistance`, moved out of the project reader.
  - `notes::parse` and `notes::write` take the track's map. Errors for drum names: an unknown one gets "did you mean" and, in the first such error only, the map's names; `Kick` is told to be lowercase; a number like `36` is told its name and note name (on any track).
  - `project.json`: drums tracks and their maps, `hocket:drum-sampler` and its `kit`, each with their own required keys, and errors for a `drumMap` or `kit` where it doesn't belong. A broken or missing map stops its track's clip files being read, rather than piling up errors in them, and an error says which clips weren't checked, so a short list isn't taken to mean they're fine.
  - The linked-file check in the reader and in `write_clip`, naming the notes, the names and the tracks (`project::drumNameConflicts`). Files are written with their first track's map (`drumMapOfFile`).
  - The overview shows a drums track's kit and map (`kick=36, snare=38`); `read_clip` says where the names come from; `write_clip`'s description has drum names. The fingerprint changed, so `hocket-mcp` was reinstalled.
  - `EditSync` gives a drum sampler no plugin, so a drums track is silent until step 7, and `--play` says so.
  - `tests/fixtures/drum-project`: the standard map, a kit that doesn't exist, linked clips, a note the map doesn't name. It loads in canonical form, and `--check` passes on it in a non-ASCII folder.
- Left for later: a missing kit (step 7, decided); drum-map commands (step 4); `hocket-stress` on a drum clip (Loose ends).

**4. Commands for the song's structure** (the bus) — planned with Lee on 2026-11-12; in SPEC §3.1, §4.1, §4.4, §5.2, §6.2 and §8.4
- Lee's decisions (2026-11-12):
  - **A time-signature change** keeps every clip and note at the same time. It's refused if any clip wouldn't then be a whole number of the new bars (`@length` is in bars), and the message lists those clips and lengths that would work. 4/4 to 2/4 or 8/8 always works; 4/4 to 3/4 works on an empty song, or when the clips happen to fit. When a change goes through and leaves clips starting off a bar line, the result lists them. The 999-bar limit applies in the new bars.
  - **Kit assignment** (built in step 7, when kits have manifests): see step 7. In step 4, setting a kit only sets its name.
  - **A track's type stays as created** in v1. Moving its clips to a new track of the other type does the same job.
  - **Transport** (seek, loop region, loop on and off) moves to step 6, with its engine side. Step 4 is only project edits.
  - **Commit after each part** of the build order below, updating PROGRESS.md as each part lands.
  - **Track names** (asked at the start of the step): one line of text, 1 to 100 bytes of UTF-8 with no control characters, in the reader and the bus alike (SPEC §4.4). The reader took any string before.
  - **New clip IDs** (asked at the start of the step): a new clip's from its track's (`clp-drums`, then `clp-drums-2`); a copy's or linked clip's from the clip it came from (`clp-verse` → `clp-verse-2`, `clp-verse-2` → `clp-verse-3`) (SPEC §4.1).
- Commands, each validated with errors written for an LLM reader, undoable, and usable in transactions (split by another author's edit) just as `write_clip` is:
  - **Song:** set the tempo (20–999); set the time signature.
  - **Tracks:** add (type midi or drums, name, position, instrument); remove, with its clips; rename; move to a position; set the instrument (none, `hocket:synth`, or `hocket:drum-sampler` with a kit name, on drums tracks only); set the mix (any of volume, pan, mute, solo); set the drum map, replacing it whole.
  - **Clips:** add one, which is empty with a length, from `.notes` text, linked to an existing clip, or a copy of one (the GUI's duplicate); move one to a new start, and optionally another track; remove one; make a linked clip unique, giving it its own copy of the file.
  - **Resizing a clip is `write_clip` with a new `@length`.** No separate command. It changes every clip linked to the file, a longer `@length` must fit each of them (already checked), and a note can't start at or after the new end, so a shorter one must leave those notes out of the text. Milestone 2's drag: Loose ends.
- How they behave (proposed by Claude, approved):
  - **IDs:** a command may give the new track's or clip's ID; otherwise Hocket makes one from the name (`trk-drums`, then `trk-drums-2`), and the result says which. Device IDs come from the track's (`dev-drums-kit`, `dev-bass-synth`); mind the 64-character limit. A new clip's file is named after its ID (`clp-verse-2` → `clips/verse-2.notes`) unless the command names one, and so is a copy's or a clip made unique.
  - **A new drums track** gets `notes::standardDrumMap()`.
  - **Removing:** a file no clip uses any more leaves `notesFiles` (the invariant in `Project.h`), and undo brings it back. What saving does with it on disk is step 9's question.
  - **The same rules as `project.json`:** IDs, clip file names, no overlaps on a track, the 999-bar song, drum names and kit names, the linked-file naming rule (moving a clip to another track, a map edit). The checks move out of `ProjectReader.cpp` into shared functions, so the reader and the bus can't drift apart. A map edit never changes notes, only how they're written, so renaming a piece rewrites every clip using it.
  - **History descriptions** say what changed: `added track trk-drums "Drums"`, `removed track trk-x and its 3 clips`, `moved clp-bass-2 to 9.1 on trk-lead`, `set trk-bass's volume to -6dB`, `set the time signature to 6/8`, `changed trk-drums's drum map: renamed snare2 to rim`.
  - **Gestures** (SPEC §3.1): a command can carry a gesture ID. Changes in a row with the same ID, from the same author, to the same setting merge into one entry, which undoes to the value before the first. A gesture that ends where it started leaves no entry. Undo, another author's edit or another setting ends the merge. No start or end calls, so nothing stays open if a client disconnects mid-drag. Volume, pan and tempo use it; the GUI in Milestone 2.
  - **Change notices** grow from `NotesChanged` to name each track, clip and song setting that changed, for steps 5 and 6 to touch only that.
  - No client can send these commands until step 10's tools, and steps 5 and 6 sync them, so `--play` can't fall out of step meanwhile. But `EditSync::notesChanged` asserts that every clip using a file is in the Edit: a `NotesChanged` for a clip a step 4 command added would trip it. Engine tests mustn't send structural commands to an `EditSync` before step 5.
- Build order, committing each part:
  1. The shared checks, out of the reader. Done: `project/Rules.h` has IDs, track names, tempo, track and device types, where a drum sampler goes, kit names, building a drum map from entries (duplicates included), clip files and the 999-bar song. Transaction names use its `isOneLineText`, so they must be valid UTF-8 now too. Overlaps stay in the reader for now: the bus needs a different check (where a moved clip would fit), written with the clip commands.
  2. Edits with their inverses (`CommandBus::Edit` grows past `SetNotes`), and the change notices. Done:
     - `commands/Edits.h`: ten edits, each returning its exact inverse (a file's notes set, added or removed; tempo; meter; a track inserted, removed, moved or its settings set; a clip inserted, removed or its start and file set). Commands are planned into them by free functions (`commands/Planning.h`; `write_clip`'s is in `ClipCommands.cpp`), so the bus keeps only history and transactions.
     - `commands/Changes.h`: a change notice for each kind of change, named by ID. Listeners get them once all of a command's (or undo's) edits have applied, so they never see a half-applied command.
     - `changesTo (file)` (for `--watch`'s warning) now also counts entries that change how a file is written, not only its notes: a time signature, a drum map. Each entry keeps the files whose canonical text it changed (`project::clipFileText`, before and after).
     - The command types moved to `commands/Commands.h`. The new edits get their tests with the commands that make them (parts 3 to 6).
  3. Song: tempo and time signature. Done (`SongCommands.cpp`, tests in `SongCommandTests.cpp`):
     - `SetTempo { bpm }` and `SetTimeSignature { "6/8" }`. A time-signature change is a `SetMeter` and a `SetNotes` for each file whose `@length` changes. It's refused when a clip wouldn't be a whole number of the new bars, naming the clips (linked ones together) and the shortest length that fits both ("a multiple of 3bars fits (3bars of 4/4 is 4 bars of 3/4)"), or when a clip would be over 999 bars or end after bar 999 in the new bars. Clips it leaves off a bar line are listed after the entry.
     - Errors start with what was being done ("set the tempo: 1000 is out of range: ..."), not a tool name, since the tools come in step 10.
     - `tests/core/BusTesting.h`: helpers for the bus's tests (`ChangeLog`, `checkReadsBack`).
  4. Tracks: add, remove, rename, move, mix. Done (`TrackCommands.cpp`, tests in `TrackCommandTests.cpp`):
     - `AddTrack`, `RemoveTrack`, `RenameTrack`, `MoveTrack`, `SetMix` in `commands/Commands.h`, planned in `TrackCommands.cpp`; `project::findTrack`, `whatHasId`, `unknownTrackMessage`; `Rules`' `hyphenatedWords` and `unusedId`. The bus's `plan` now dispatches any command with a `planning::plan` overload.
     - A track's place is `TrackPlace { before, after }`, a neighbour's ID, never an index (CLAUDE.md). Adding with neither puts it last; a move must name one.
     - Device IDs cut the track's words short, not the role, to fit 64 characters (`dev-<track>-synth`, `dev-<track>-kit`). An added track's result names its device, and a drums track's says it has the standard map.
  5. Clips: add (each way), move, remove, make unique. Done (`ClipCommands.cpp`, tests in `ClipCommandTests.cpp`):
     - `AddClip { track, start, content, id, file }` whose content is `EmptyClip { bars }`, `ClipText { notes }`, `LinkedTo { clip }` or `CopyOf { clip }`; `MoveClip { clip, start, track }`; `RemoveClip`; `MakeClipUnique { clip, file }`.
     - `Rules`' `clipPlacementProblem` checks the 999-bar song and overlaps for the bus, and says where the nearest room is on either side ("The nearest starts where it fits are 3.1 and 6.1"). The reader keeps its own overlap check, which reports a clip that starts inside another, at its place in the file.
     - New files are named after the clip's ID, numbered if taken (`unusedClipFile`). A named file another clip uses is refused, pointing at a linked clip instead. A linked clip can't name a file.
     - Moving a clip to another track checks that every track still using its file names its notes alike. Moving it to a track of the other type says how its notes are written there.
  6. Drum maps and instruments. Done (`TrackCommands.cpp`, tests in `TrackCommandTests.cpp`):
     - `SetInstrument { track, instrument }`: none, or one. Changing only a drum sampler's kit keeps its device and ID; another kind of device gets a new ID and the history says which it replaced. Setting a kit only names it until step 7.
     - `SetDrumMap { track, entries }` replaces a drums track's map whole, built with the reader's `buildDrumMap`, so every problem is listed at once. The history says what changed (`renamed snare2 to rim`, `moved clap from 39 to 41`, `added perc (82)`, `removed snare (38)`; after four, "and N more changes"), and the result names the clips now written differently. A file shared with another track must still mean the same notes on each.
  7. Gestures. Done (in `CommandBus`, tests in `GestureTests.cpp`): `SetTempo` and `SetMix` take a `gesture` ID. The bus remembers the one entry a gesture may still merge into; a merge keeps the entry's inverse (the value before the first step) and takes the latest step's edits and description, and the entry goes when the two match. Any other new entry, undo or redo ends it; inside a transaction, steps just join the transaction. Each step still announces its change, so the engine follows the drag. Edits gained `==` for this.
  8. Random command sequences. Done (`CommandSequenceTests.cpp`): 200 seeds of 40 steps, every project command with valid and invalid arguments, plus undos and redos. A refused command changes nothing; after any other, every clip's file is in the project and nothing else is, and the project writes, reads back and writes the same. Undoing everything gives the original, and redoing everything the same end state. Each command kind is applied and refused at least 20 times; it runs in about a second.
- No client sends these commands yet (step 10), and `EditSync` still only handles `NotesChanged` (steps 5 and 6).
- Tests:
  - Each command: success, its history description, undo, redo, and every error message's wording.
  - In a transaction, and split by another author's edit.
  - Random command sequences, invalid ones included, which must change nothing: after each command the project writes and reads back unchanged (`project::write`, then `parse`); undoing everything gives the original; redoing everything gives the same end state.
  - Gestures: merging; another author or setting ending it; undo mid-gesture; a gesture back to its start leaving no entry.

**5. Sync: tracks and clips** — planned with Lee on 2026-11-12
- Lee's decisions (2026-11-12):
  - **When a change ends the song before the playhead** (the last clip removed while it plays, say), the playhead wraps as if the loop had always been that length, landing on the same beat of the bar so the pulse carries on. If that isn't clean in Tracktion, say why, and jump to 1.1 instead. [Claude's note: Tracktion's playhead maps its running sample count into the loop by remainder, which may already do this when playback started at the loop's start. Check its source and test it live.]
  - **Clip changes must be glitch-free.** If adding or removing a track or an instrument can't be made gapless, stop and tell Lee what was measured before working around it.
- How it works (proposed by Claude, approved):
  - `EditSync` keeps Tracktion's tracks by ID, as it does clips, not by their index in the project.
  - Each change notice names a track or clip; `EditSync` brings that one in line with the model as it now is, whatever the notice says, so the order of a command's notices doesn't matter (a clip moved between tracks is "removed", then "added").
  - Handled: tracks added, removed, moved and renamed; instruments added, replaced or removed (a 4OSC made or deleted); clips added, removed, moved, or given a new file; files joining or leaving the project. The assertion that clips never appear or vanish goes.
  - The loop range is recomputed after every structural change.
- Build order, committing each part:
  1. Sync by ID, with structural tests: after each command, the Edit's tracks, plugins and clips match the model. Step 4's random command sequences run against a live `EditSync` too (fewer seeds: 4OSC is slow to create). Done (`StructureSyncTests.cpp`):
     - `EditSync` makes its tracks itself (`numAudioTracks = 0`) and keeps them by ID with the plugins their devices made; `syncTrack`, `syncClip` and `syncFile` each bring one thing in line with the model, making, changing or removing it. A clip moved to another track is removed and made again there.
     - Tracks go after the Edit's own tracks (tempo, markers), in the model's order; `syncTrackOrder` moves any that are out of place.
     - The tempo, time signature and mix still don't reach the Edit (step 6), so the random sequences here leave them out. The generator moved to `tests/common/RandomCommands.h`, shared with the core tests.
     - 4OSC turned out quick to create: 20 random sequences of 30 steps, each checked against the whole Edit, take under a second. Its slowness is in rendering (Loose ends).
  2. Offline renders: clips added, moved, removed, linked and undone are heard where the model says. Done (`RenderTests.cpp`, "render:"): which bars start with a note, after clips are added (a copy, a linked clip), moved across tracks, removed and undone, and after a track or instrument is added and removed. Tracktion won't render an Edit in which nothing makes a sound, so those tests keep one synth.
  3. Live tests, the riskiest first: a clip removed or moved while its note sounds ends that note (Tracktion's `LoopingMidiNode` hands notes on only when the node survives a rebuild, and a removed clip's doesn't); a change to one track doesn't stop or retrigger a held note on another; the playhead wraps when the song gets shorter. `MidiRecorder` moves to the shared test helpers.
  4. Stress tests, one for each kind of change at 20 a second during playback: clips moved, added and removed; tracks added and removed. Each has Milestone 0's checks: no block over budget, no dropped block, gap, click, retriggered note or stuck note. If 4OSC's set-up (it builds its wavetables each time) keeps the message thread from managing 20 tracks a second, that delays commands but needn't glitch audio: then device-less tracks at 20 a second, and a slower run with synths, saying so.

**6. Sync: mix settings, tempo, time signature, transport**
- Volume and pan through Tracktion's volume plugin, smoothed so that a jump doesn't click; mute and solo (SPEC §4.4: a muted track stays silent when soloed). Then take out `--play`'s "doesn't apply ... yet" note.
- The pan law is SPEC §4.4's balance law, which Tracktion doesn't have. Its linear law (`PanLawLinear`: left `g(1 − p)`, right `g(1 + p)`) gives it exactly: for a balance pan `b` to the right, use Tracktion pan `p = b / (2 − b)` and gain `g = volume / (1 + p)`, and the mirror image to the left. Set the law per plugin (`VolumeAndPanPlugin::panLaw`), not engine-wide. A render test checks both channels' gains at C, L50 and R100.
- Tempo and time-signature changes during playback: the playhead keeps its place in bars, and held notes aren't retriggered.
- Transport, moved here from step 4 (Lee, 2026-11-12): bus commands for seek, the loop region, and loop on and off; `commands::Transport` grows to match, and `EditSync` carries them out. Not history entries, and they don't split another author's transaction (SPEC §3.1).
  - To decide: what play does with a loop region (today it loops the whole song), and whether the region is saved with the project or only lasts the session. SPEC §4.4 has no key for it.
- Stress tests: volume and pan changed 20 a second with no click, mute toggled, tempo changed.
- **A master level for `--play`** (Lee, 2026-11-12; Loose ends, the loudness item): playback shouldn't reach the device at full scale while there's no master bus (Milestone 5). Through Tracktion's master volume plugin, with a conservative default and an option to change it, since this step wires volume into Tracktion anyway. To pick then: the default (Claude proposes −12 dB, checked against an offline render of the demo's peak level) and the option (e.g. `--master-volume -6dB`, read with `units::volume()`). Not saved in the project: Milestone 5's master bus is.

**7. Hocket's drum sampler, and how kits work** (SPEC §8.1, §8.4)
- Hocket's first device, a Tracktion plugin type of its own (`PluginManager::createBuiltInType`). It sets the pattern the synth and effects follow in Milestones 4 and 5.
- One-shot pieces; velocity sets the level. Voices are allocated up front, samples are loaded on the message thread and handed over without locks, and nothing on the audio thread locks or allocates.
- Not Tracktion's `SamplerPlugin`, which locks and allocates on the audio thread (Known upstream issues).
- Kits: in the repository under `assets/kits/<name>/`, each with a small manifest (notes, files and levels, and names for any pieces beyond the standard ones), and installed next to the programs. A project names its kit; nothing is copied into the project. A test checks that every bundled kit uses the standard names on the standard notes (SPEC §5.2).
- Tests use generated samples, not a real kit: hits land where the clip says, at the right level; a drum clip rewritten 20 times a second plays with no glitch, on the real-time harness with `MidiRecorder`.
- A missing kit (Lee, 2026-11-12; SPEC §8.4): a project whose kit isn't installed still loads, `--play` and the overview say the kit is missing, and the track is silent.
- Assigning a kit (Lee, 2026-11-12; SPEC §5.2): every entry already in the track's map is kept, and the kit's names are added for notes and names the map doesn't use yet, so nothing renamed or added is lost. The result says which of the kit's pieces were added to the map, and which weren't and why (the note already has another name, or the name is on another note). A new drums track with a kit gets the standard map merged with the kit's names the same way.
- What the kit can't play (Lee, 2026-11-12): a track's map will usually name more than its kit has (the standard map names 47 pieces). The overview and `write_clip`'s result should say which of the map's names, and which notes in the clip, the kit can't play. Here for `write_clip` and the overview as they are; step 10 keeps it in whatever tools replace them.
- Then: `EditSync` creates the sampler (it creates nothing for one now), and `--play`'s "is silent" note goes. `write_clip`'s note about unnamed notes says, for each, whether the kit has a piece there.

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
- Tools for steps 4 and 9: tracks, clip placement, tempo and time signature, transport with seek and loop, and `save`. Also `patch_clip` and `history`'s author filter. The overview already shows mix settings, links, drum maps and kits.
- Results carry what the commands say: the IDs Hocket made, clips a time-signature change left off a bar line, kit pieces added or not on assignment, and what a track's kit can't play (step 7). Resizing stays `write_clip` with a new `@length`; its description should say so.
- The set is designed at the start of the step, small and expressive as SPEC §6.4 asks, and written into §6.4.
- Descriptions, results and errors are written for an LLM reader. The fingerprint changes, so `hocket-mcp` must be installed again (CLAUDE.md).
- API tests for each tool; the bridge's and stress client's tests updated.

**11. Demo song, acceptance run, SPEC updates**
- The acceptance check itself: starting from `--new`, Claude builds a song with drums while Lee listens. It's saved as `examples/m1-demo`.
- `hocket-stress` and `Run-Acceptance.ps1` get a structural mode: clips moved and tracks muted during playback, silently, on clips left empty. `hocket-stress` also needs to rewrite drum clips (Loose ends).
- SPEC gets what Milestone 1 taught. A fresh-clone run.

## What exists
- `src/core` (`hocket_core`, no JUCE):
  - `notes/`: the `.notes` model, parser and canonical writer. A clip is at most 999 bars (`maxLengthInBars`). `Meter` is the time signature (numerator 1–32, denominator 2/4/8/16; ticks per beat, bar and sixteenths per beat), with `parseMeter`. `parse` and `write` take a drums track's `DrumMap` (names both ways, `drumNameProblem`, `nearestNames`, `standardDrumMap()`).
  - `project/`: the `project.json` model, reader, writer, and lookups like `findClip`, `notesOf`, `clipsUsing`, `tracksUsing` and `songEnd`. Each clip file's notes are kept once in `Project::notesFiles`, so linked clips share them. Tracks are `midi` or `drums`, with volume, pan, mute and solo; a drums track has a `drumMap` (`drumMapOf`, and `drumMapOfFile` for a file's), and a drum sampler a `kit`. `drumNameConflicts` says where a linked file's tracks would name its notes differently. A song ends by 1000.1 (`maxSongLengthInBars`). `ClipFileWatcher` for `--watch` watches each file once, and keeps trying a settled file it can't read yet, for up to 2 s.
  - `project/Rules`: the rules a valid project keeps, shared by the reader and the bus (step 4).
  - `Suggestions`: `editDistance`, for "did you mean".
  - `units/`: values with units (`units::parse`/`format` for any `Quantity`; `units::volume()`), and pan (`parsePan`/`formatPan`).
  - `commands/CommandBus`: the only way to change a `project::Project`. Authors are `Author { name, session, label }`, compared by name + session. A command that changes nothing adds no entry. `interruptTransaction`, `openTransaction`, `changesTo` (the entries that changed a clip file's text: its notes, or how they're written); gestures merge into one entry (SPEC §3.1); it also routes transport commands to the engine, and `isPlaying()` asks it.
  - `commands/Commands.h`: the commands. `WriteClip`; `SetTempo`, `SetTimeSignature`; `AddTrack`, `RemoveTrack`, `RenameTrack`, `MoveTrack` (by a neighbour's ID), `SetMix`, `SetInstrument`, `SetDrumMap`; `AddClip` (empty, text, linked, copy), `MoveClip`, `RemoveClip`, `MakeClipUnique`; `Play`, `Stop`. Values are written as project.json writes them.
  - `commands/Planning.h`: each command validated against the project and planned into edits (`SongCommands.cpp`, `TrackCommands.cpp`, `ClipCommands.cpp`). Errors start with what was being done ("add a clip: ...").
  - `commands/Edits.h`: ten edits with exact inverses, which history entries keep. `commands/Changes.h`: change notices by ID, delivered once a command's edits have all applied.
  - `commands/WatchedSaves`: `--watch`'s saves as the person's `write_clip` to the file's first clip, with a warning when one replaces a change made some other way since the file and its notes last matched.
  - `Paths`: UTF-8 ⇄ path (`utf8FromPath` never throws: an unpaired UTF-16 surrogate becomes U+FFFD), `readTextFile`.
- `src/platform` (`hocket_platform`): `Platform.h` with `Platform_windows.cpp` / `Platform_posix.cpp`: the command line as UTF-8 (`utf8Arguments`, used by both programs), binary stdin/stdout, secure random bytes, process ID and program name, "is this program still running", `%LOCALAPPDATA%`, `FileLock` (one holder at a time; on Windows nothing else can even read the file), and the TCP port probe. The POSIX file has never been compiled.
- `src/engine` (`hocket_engine`):
  - `EditSync` builds a Tracktion `Edit` from the bus's project in any time signature (a 4OSC for each `hocket:synth`, nothing yet for a `hocket:drum-sampler`), rewrites every Tracktion clip using a file on each `NotesChanged`, and is the bus's `Transport` (play loops the whole song). Mix settings don't reach it yet (step 6). Its clips don't use Tracktion's proxy sequences, so they play through `LoopingMidiNode`, which ends the notes a rewrite takes away (Gotchas). `stoppedByItself()` tells a stop Tracktion made from one a transport command asked for.
  - `PlaybackDevice.h`:
    - `openDevice (DeviceChoice)`, from `--driver`, `--device`, `--sample-rate` and `--buffer-size`. It refuses a rate or size the device doesn't offer before changing anything, and catches a driver that carries on as it was. Errors suggest a name when exactly one contains the text given.
    - `listDevices` for `--list-devices`. Only the driver or device named has its rates and sizes listed, since that loads the driver. `formatNumbers` writes evenly spaced runs as ranges.
    - `PlaybackDevice`: what `--play` says about the device at the start and at Ctrl+C. It asks JUCE's device manager each time, since the manager deletes a device that disappears and opens another. The xrun report has the driver's count (where it keeps one) and JUCE's count of audio callbacks that took longer than their block.
- `src/api` (`hocket_api`, no JUCE): the WebSocket API (SPEC §6.3), its server and its client.
  - `Handler`: JSON-RPC in, reply out; `hello` with the token and the build check (error -32002 for another build); per-client state; interrupts on disconnect and in `checkIdle()`. It never throws: anything thrown while handling a request is answered with -32603.
  - `Protocol`: what both ends share: `protocolVersion` (bump it for changes to `hello` or the formats), `defaultIdleTimeout` (10 minutes), `hocketVersion()`, `buildFingerprint()` (FNV-1a of the protocol version and `toolDefinitions()`), and `makeSessionId()` for clients.
  - `Client`: a blocking IXWebSocket client, one request at a time; a late response to a timed-out request is dropped. Used by hocket-mcp and hocket-stress.
  - `Tools`: the 9 Milestone 0 tools, their definitions and argument checks. The overview and `read_clip` show mix settings, linked clips, drum maps and kits, and what a beat is outside x/4. On a drums track, `write_clip` lists the notes the map doesn't name.
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
- Tests (`ctest`, 13 tests, 40 to 60 s, most of it the stress tests running in real time; 48 s on 2026-11-12):
  - `tests/core` (`hocket_core_tests`): 168 cases, drum maps and drum clips among them, with property tests through the standard map. It links `hocket_platform` for `FileLock`, which holds a clip file the way an editor might.
  - `tests/engine` (`hocket_engine_tests`, own main, one shared Tracktion `Engine`): 37 cases, about 40 s (the whole `ctest` took 46 s on 2026-11-12). It took 53 s once, on 2026-11-06, with the laptop busy (47% CPU load, 2.4 GB of memory free): the time went to the live tests, about 3.7 s each instead of 0.5 s.
    - Edit structure (6/8, linked clips, a track with no device); offline renders (notes start where `.notes` says, in five time signatures; `write_clip` and undo move them); a stop no command asked for.
    - `PlaybackDevice` against a fake JUCE device type that a test can unplug, and whose rates, sizes and stubbornness a test picks; it never opens a real device. Choosing rates and sizes, suggestions, `listDevices`, `formatNumbers`.
    - Live playback through Tracktion's hosted audio device: looping, a mid-loop rewrite heard by the next pass, a sounding note swapped for another (the old one ends, the new one waits for the next pass), a held note not retriggered by a rewrite, plus a control showing that check catches a retrigger.
    - Three stress tests (`StressTests.cpp`, about 30 s) in a real-time harness. An `AudioThread` plays `FakeAudioDevice::playBlock` at a sound card's pace with real-time priority, timing each block, while the test thread is the message thread.
      - 200 comment-only rewrites at 20/s to a held note. Checked: no block over its 5.8 ms budget (Release only), and no gap, retrigger or click against the first, untouched pass. A dropped block, a whole block of silence, fails it too. A control damages a copy to show each check can fail.
      - 21 rewrites that move a note, with no block over budget, and the last rewrite is what plays.
      - `hocket-stress --vary`'s rewrites of the demo line (200, every other one an octave up), then an empty clip: silence afterwards, and no note left on in the MIDI reaching 4OSC. `MidiRecorder`, a test plugin put ahead of 4OSC, records that MIDI without allocating.
  - `tests/api` (`hocket_api_tests`): 53 cases. The handler driven directly (hello, the token and the build check, every tool, argument errors, interrupts on disconnect and idle with a fake clock, notifications included, internal errors), the fingerprint, `TextChanges`, the token file, the instance lock and other platform functions, and end-to-end tests: an IXWebSocket client talking to the server on 127.0.0.1 (ports from 46300), with the test thread as the message thread.
  - `tests/mcp` (`hocket_mcp_tests`): 10 cases. The bridge against a real `Server` and `Handler` on a thread of their own (`FakeHocket`, ports from 46320, token file in a non-ASCII temp folder). Covers: tools listed without Hocket; not running (no file, a stale file, nothing listening); write, undo and history as the client's own author; names and labels; a restart (same session, note once); another build; a timeout with a late response dropped; protocol errors.
  - `hocket_mcp_stdio` (CTest, `tests/mcp/StdioTest.cmake`): runs `hocket-mcp.exe` on piped stdin with a byte order mark, a blank line and a CRLF line. Checks one LF-terminated JSON reply per request and nothing else on stdout, and that the non-ASCII `--token-file` and `--label` arrive intact.
  - `tests/stress` (`hocket_stress_tests`): 5 cases. The rewrites' text (steady and varied); a run against `FakeHocket` (ports from 46340): paced, all applied, the clip put back, the history; errors before starting; the summary. `hocket_stress_not_running` (CTest) runs the program with a non-ASCII `--token-file` that doesn't exist.
  - `tests/app`: CTest end-to-end runs of `hocket-headless` on a non-ASCII folder, the demo's and the drum fixture's.
  - `tests/fixtures/drum-project`: a drums track with the standard map and a kit that doesn't exist, linked clips, and a bass track.
  - `tests/acceptance` (`acceptance_checks`, CTest, Windows PowerShell): `Get-StopResults` on made-up `--play` logs and exit codes: clean stops, xruns, a crash before and after the `stopped` line, a `--play` that stopped early or ignored Ctrl+C, a token file left behind.
  - `tests/common`: `TemporaryFolder.h` (non-ASCII temp folders) and `FakeHocket.h` (a bus, handler and API server on a thread of their own, plus a token file: a running Hocket, as far as a client can tell).
- `examples/m0-demo`: 120 BPM, 4/4, one track `trk-bass` with a 4OSC `dev-bass-synth`, one 2-bar clip `clp-bass` at 1.1.
- **Threads**, as the code review of 2026-10-31 found them:
  - No Hocket code runs on the audio thread. `EditSync` changes the Edit on the message thread; Tracktion builds the new graph there and destroys the retired one there too, after its pool threads let go (`LockFreeMultiThreadedNodePlayer::postNewGraph`).
  - IXWebSocket's threads only post to the message thread; the bus, handler, the server's connection map and `EditSync` are only used there; `Client`'s shared state is behind its mutex; the Ctrl+C flag is atomic.

## Set up on Lee's laptop
`hocket-mcp.exe` is installed in `%LOCALAPPDATA%\Hocket\bin` and registered in Claude Code as `hocket` at user scope (API build `763b8f444d8d3b0e`). Every Claude Code session gets the tools; they say "Hocket isn't running" until `--play` runs. After a change to the tools or the API, run CLAUDE.md's install command again and restart Claude Code sessions.

**To do when Lee sets up Claude Desktop** (not started; don't drop this):
1. Register the installed `hocket-mcp.exe` in Claude Desktop's config. It should be `%APPDATA%\Claude\claude_desktop_config.json`, under `"mcpServers": { "hocket": { "command": "<path to hocket-mcp.exe>" } }`; check against Claude Desktop's own docs. Then restart Claude Desktop.
2. Find the `clientInfo` name it sends. hocket-mcp logs `client '<name>' <version>, MCP <version>` to stderr on `initialize`, and Claude Desktop keeps servers' stderr in its MCP logs (expected in `%APPDATA%\Claude\logs\`, e.g. `mcp-server-hocket.log`). `--play`'s `API: ... connected` line shows the name Hocket ends up using.
3. Map that name to "Claude" in `mcp::authorName` (`src/mcp/Bridge.cpp`). Add a check to "names and labels are made to fit hello" in `tests/mcp/BridgeTests.cpp`. Update SPEC §6.3's `client` line and the "Other MCP clients" section of `docs/connecting-claude-code.md`. Rebuild and reinstall.
4. Check the label too. Claude Desktop isn't started in a project folder, so the working folder's name may mean nothing (an app or system folder). If so, pass `--label` in its config, or pick a better default for clients that don't start the bridge in a project folder.

## Test hardware
- **Laptop** (main development machine): Magni Mesh DAC, used at the desk. It has no ASIO driver of its own. ASIO4ALL buzzes on it at 512 samples but is clean at 1024 (Loose ends); FL Studio ASIO works on the Magni at 512, so the ASIO checks use it here. Away from the desk, headphones go on the Realtek; FL Studio ASIO wouldn't start there on 2026-11-12, but Realtek ASIO works. Device names as `--list-devices` prints them: `FL Studio ASIO`, `ASIO4ALL v2` and `Realtek ASIO` under ASIO; the Magni under Windows Audio is `Speakers (Magni Mesh Dac)`. None of these drivers counts xruns (SPEC §9).
- **Desktop**: Focusrite Scarlett 2i2 with Focusrite's native ASIO driver and mic inputs, the test interface for audio recording (Milestone 7). Lee's Yamaha piano connects to it for MIDI input and recording (Milestone 3).
  - **It needs the toolchain by Milestone 3**: Visual Studio 2026, Git, a clone with its submodules, and a build that passes its tests (CLAUDE.md), then `hocket-mcp` installed and registered there. Check its free memory and disk first (Gotchas: build time).

## Loose ends
- **ASIO4ALL v2 on the Magni buzzes at 512 samples (2026-11-04; cause found 2026-11-12).** Sharp, buzzy "NES helicopter" sounds came with the bass line. It's the buffer size: at 1024 samples (`--buffer-size 1024`) it plays cleanly. Not the sample rate: it still buzzed at 48000 Hz, the rate Windows runs the Magni at.
  - FL Studio ASIO plays cleanly at 512 on the same DAC, so Hocket keeps up at 512 in general. ASIO4ALL streams to the device directly (kernel streaming), while FL Studio ASIO goes through Windows' audio engine and its buffering, so ASIO4ALL most likely needs more room with this USB DAC. The late-callback count at 512 wasn't noted; if it ever matters, a nonzero count would point at Hocket instead.
  - For Milestone 2's audio settings: the person picks the buffer size (planned, SPEC §9), and Hocket could suggest a bigger one when it hears of trouble: late callbacks, or a driver like ASIO4ALL on a USB device. To decide then.
  - Tracktion saved the last device used, so the next `--play` without options opens whatever Lee ran last (ASIO4ALL at 1024, or FL Studio ASIO).
- **Playback can be dangerously loud through drivers that bypass Windows' volume (Lee, 2026-11-12).** The ASIO4ALL check on the Magni was far louder than a YouTube mix playing through Windows at the same knob setting. ASIO4ALL streams straight to the device, so neither the Windows volume nor any app's applies, and Hocket has no master level yet (Milestone 5): the demo's bass reaches the DAC at whatever level 4OSC makes it.
  - Lee's decisions (2026-11-12), rather than waiting for Milestone 5:
    - **Done:** `--play` warns, when it opens an ASIO driver (or Windows Audio in exclusive mode), that the driver may bypass the Windows volume slider, so the device's own volume control sets the level, and to turn it down (`engine::volumeWarning`). It comes as playback starts, so it helps most the next time.
    - **Step 6:** `--play` gets a master level with a conservative default (see step 6). The full master bus still comes in Milestone 5, and should keep a safe default too.
  - Still a proposal: Milestone 2's audio settings could say the same when an ASIO driver is picked.
  - Until the master level is in, any check over ASIO starts with the Magni's knob well down.
- **FL Studio ASIO wouldn't start with headphones on the Realtek (2026-11-12).** "Device didn't start correctly", with or without `--sample-rate`. On the Magni it opens and plays (checked 2026-11-12, `--sample-rate 44100`), so the Realtek setup was the cause, not Hocket. The saved device is FL Studio ASIO again. If it matters away from the desk, its output setting (`HKCU\Software\Image-Line\ASIO`, `outputEndPoint`) is blank, which seems to mean the Windows default output.
- **Milestone 2: dragging a clip shorter than its notes** (Lee asked for this to be noted, 2026-11-12). Resizing is `write_clip` with a new `@length` (step 4), and a note can't start at or after a clip's end (SPEC §5.4), so a drag that cuts into a clip's notes must either remove them or stop. To decide with Lee then. [Claude's proposal: the drag removes the notes that would start past the new end, in the same undo step, and the clip shows which while dragging; undo brings them back. On a linked clip it changes every clip using the file, so the drag should show that too. The alternative is a drag that stops at the bar holding the last note.] Notes that start before the new end but run past it are kept and cut off at playback.
- **`hocket-stress` can't rewrite a drum clip yet (2026-11-12).** It parses `read_clip`'s text with no drum map, so a clip using drum names fails with "couldn't parse clip". The fix: pass the names through untouched, since the default rewrite only adds a comment, and have `--vary` move notes by name or give it the map. Step 11's structural mode is the time to do it.
- **A per-track stereo width control** (added 2026-11-12; planned for Milestone 5, and needed by Milestone 7's stereo audio). It runs from 0% (mono) to 100% (unchanged), and applies before pan. Pan is a balance control that never moves one channel's content to the other side (SPEC §4.4). So width is how a stereo source, like a stereo recording, gets narrowed before it's panned. To decide then: its `project.json` key and how it's written, e.g. `"width": "100%"` through the units parser.
- **No text selection while `--play` runs (2026-11-04).** Not being fixed: the GUI replaces `--play` for everyday use (decided 2026-11-06). The cause is the review's fix #10, which turns QuickEdit off, so that a selection can't pause console output and with it the message thread. Shift+drag should still select in Windows Terminal. If it's ever needed: write console output from a thread of its own, through a queue, and leave QuickEdit as the user had it.
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
- **Command bus:** a merged gesture's history entry takes its latest step's description, which names only what that step changed: a drag of volume and pan together whose last step moved only the volume says only "volume". SPEC §6.4 has no MCP `redo`; if one is added, it gets the same author check as `undo`, applied to whoever did the undo.
- **Sync coverage:** `EditSync` handles `NotesChanged` only, and asserts that clips are never added or removed; step 4's other change notices are ignored until steps 5 and 6. Each new command that changes the project needs its sync code and a render or live test, and each new kind of change to the Edit a stress test (SPEC §3.3).
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
  - Checked-out sources are CRLF, but files the Write tool creates are LF, so `git add` warns "LF will be replaced by CRLF". That's harmless: the repository stores LF either way. A script that edits a source file should keep its line endings (read with `newline=''` in Python).
- **Building from the tools.** `cmake` isn't on PATH in the PowerShell tool. Prepend `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` to `$env:PATH` in the same command, then use CLAUDE.md's presets. Filter a build's output with `Select-String -Pattern "error|warning C"`: no output means a clean build.
- **Test fixtures** live in `tests/fixtures/` (`HOCKET_FIXTURES_DIR` in the core tests); `examples/` is for projects meant to be played (`HOCKET_EXAMPLES_DIR`).
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
  - A heredoc whose text contains `\"` fails outright, even with a quoted delimiter: bash reports "unexpected EOF while looking for matching". For a scripted edit, write the script to the scratchpad with the Write tool and run it from there.
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
  - It also blocked a commit and push that Lee hadn't asked for in that message (2026-11-12, a CLAUDE.md edit). CLAUDE.md now says when Claude may commit and push on its own; if the check still blocks one, say so and give Lee the commands, or ask.
- **Long background runs can be killed under memory pressure.** Claude Code stops background commands when the system runs critically low on memory, even if they aren't the cause. Check free memory before a fresh-clone run or a long build, and ask Lee about memory-heavy apps first.
- **Debugging Tracktion's playback**, as with the stuck notes and the dropped block (2026-11-04 to 06):
  - Record what reaches the synth: `MidiRecorder` (`tests/engine/StressTests.cpp`) is a test plugin put ahead of 4OSC. It shows the MIDI the synth really gets, in order. Logging each block's `context.editTime` there shows skipped blocks too: a jump of exactly one block means the plugins never ran for it.
  - Make it happen often: a "hammer", a temporary test on `StressTests.cpp`'s real-time harness doing 1,000 comment-only rewrites at 100/s to a held note, turned a 1-in-1,700 bug into one every few runs. Compare 20 or more runs before and after, then take the test out again (it's slow).
  - Look inside Tracktion: add temporary logging to the submodule, appending to a static, preallocated array with an atomic index (nothing allocates on the audio thread), and read it from the test through `extern` declarations. Take it out by hand afterwards and check `git -C external/tracktion_engine status` is clean, since `git checkout` may be blocked.
  - Read the node that actually plays: a MIDI clip plays through `MidiNode` or `LoopingMidiNode` depending on `setUsesProxy` (above).
