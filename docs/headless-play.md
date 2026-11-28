# `hocket-headless --play`

What `--play` does, in full: moved here from CLAUDE.md on 2026-11-27, which keeps a summary. Read it before changing `--play`, or driving it from a tool. SPEC §4.5 and §9 have the rules behind it.

- `--play <folder>` loops the project until Ctrl+C, or until its console window is closed, which stops it just as cleanly. `--driver` and `--device` take names as `--list-devices` prints them, and `--sample-rate` and `--buffer-size` take values the device offers. Without them it reuses the last device, rate and size, which Tracktion saves in `%APPDATA%\Hocket\Settings.xml`.
- With `--watch`, `project.json` or a clip file saved outside Hocket is taken in as your command (one replacing the project, or a `write_clip`), with a warning when a file's save replaces an unsaved change made through the API; Hocket's own saves aren't taken for outside changes.
- `--user <name>` is your name in the history (default `user`).
- If playback stops by itself (Tracktion does when its device lists change), it says so.
- Drum kits load before it plays (it says `Loading kit ...` and `Loaded in ... ms`): bundled ones from `kits\` next to the programs, which the build copies from `assets\kits`, the user's from `Documents\Hocket\Kits`; a saved change to a user kit in use is reloaded and heard from the next hit, and a kit a track names that wasn't there, or was broken, is loaded once it's there, and `--play` says so.
- Over ASIO, or Windows Audio in exclusive mode, it warns that the driver may bypass the Windows volume slider, so the device's own volume control sets the level.
- Everything plays at a master level of 0 dB, the project as it's mixed, unless `--master-volume <dB>` (e.g. `-6dB`) says otherwise; renders don't include it.
- A Ctrl+C before play starts (while the device opens or the kits load) stops it with `stopped before playing`.
- About 3 s after changes stop, it writes a recovery copy of the project to `%LOCALAPPDATA%\Hocket\Recovery` (found again by the project's `.hocket/id` if the folder is moved or renamed), removed once nothing is unsaved; at exit with changes unsaved it keeps it and says where (SPEC §4.5).
- If a session left one, plain `--play` plays nothing and says what it holds: `--recover` plays it (one undoable entry, unsaved; undone without a save, the copy is filed with the kept saves), `--discard-recovery` files it with the project's kept saves and plays the files as saved. Both stop before the device opens.
- When the project's files don't load, `--play` says where the recovery copy and the kept saves are, and `--recover` starts from the copy, unsaved.
- A save a crash interrupted part-way (`.hocket/saving.json`) is finished by the next `--play` before it reads the folder, all of it or none: one it can't finish, or whose finishing fails part-way, stops `--play`, which says why and where the version before the save and the save's own are; `--check` only says so. What a save stopped before its marker left is set aside in `unused/`.
- In its console, `S` saves the project as `--user`; a save that would overwrite a file changed outside Hocket is refused, naming it, and `O`, pressed after reading that (a second or more later), saves over what it named, keeping what was on disk as a kept save; if more has changed outside Hocket since, it's refused again.
- At Ctrl+C during play it prints the driver's xrun count (where the driver keeps one), how many audio callbacks took longer than their block, and the peak output level, with a warning if the output clipped.
