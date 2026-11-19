"""Synthesizes Hocket's bundled electronic drum kit, an 808-style kit, and writes
it to assets/kits/electronic/: kit.json, a WAV per piece, and the samples'
license note (CC0). Every hit is made here from oscillators and noise; nothing
is sampled from a recording (SPEC §8.4).

    python tools/kits/make-electronic-kit.py           # write the kit
    python tools/kits/make-electronic-kit.py --check   # does the kit on disk match?

To change a sound, change its piece's parameters in PIECES below and run this
again. A run writes the same bytes every time, so git shows only the pieces a
change touched, and kit.json, whose levels all move when the kit's trim does.
Needs Python 3 and numpy.

The samples are 44.1 kHz, 16-bit, mono (Lee, 2026-11-18). Each piece is made
in 64-bit float at 4x that rate, so clipping and square waves don't alias, then
filtered down to 44.1 kHz, scaled to peak at -1 dBFS, faded to silence over its
last 20 ms, and dithered (TPDF) to 16 bits. The balance between pieces is each
piece's level in kit.json: its level_db below, plus one trim for the whole kit,
set so that every piece struck at once at v127 peaks at KIT_PEAK_DBFS, between
samples too: Hocket's headroom rule for kits, with half a dB to spare for
conversion to other rates.

Times are in ms and pitches in Hz. A decay is how long a sound takes to fall
60 dB; an attack, how long it takes to rise from silence.
"""

import argparse
import math
import os
import struct
import sys
import time
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
KIT_FOLDER = os.path.normpath(os.path.join(HERE, "..", "..", "assets", "kits", "electronic"))

RATE = 44100
OVERSAMPLE = 4
FS = RATE * OVERSAMPLE
SAMPLE_PEAK_DBFS = -1.0
KIT_PEAK_DBFS = -1.5
FADE_MS = 20.0
FADE_IN = 8  # samples, 0.18 ms
DESCRIPTION = "Electronic, 808-style, synthesized by Hocket"

# The 808's six square-wave oscillators behind its hats and cymbal.
METAL_HZ = (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)

LICENSE = """\
The samples in this folder (the .wav files) and kit.json were synthesized by
Hocket's generator, tools/kits/make-electronic-kit.py, from oscillators and
noise. Nothing in them is sampled from a recording.

They are dedicated to the public domain under CC0 1.0 Universal:
https://creativecommons.org/publicdomain/zero/1.0/

To the extent possible under law, the Hocket authors have waived all copyright
and related or neighboring rights to them. Use them for anything.
"""

# ==============================================================================
# The pieces, in note order, with the standard names (SPEC §5.2). "voice" is the
# function below that makes the sound; the rest are its parameters.

TOM = dict(voice="tom", bend=1.25, bend_ms=40, attack_ms=0.4, noise=0.08, noise_ms=60)
CONGA = dict(voice="tom", bend=1.08, bend_ms=20, attack_ms=0.3, noise=0.0, noise_ms=10)

PIECES = [
    # The driven kick: the 808 body pushed hard into a clip, so it's near-square while it's loud.
    dict(name="kick2", note=35, voice="kick", level_db=-2.0, length_ms=520,
         pitch_hz=52, sweep_from_hz=210, sweep_ms=28, decay_ms=480, attack_ms=0.3,
         click=0.5, click_hz=2500, click_ms=6, drive_db=18, clip="hard"),
    dict(name="kick", note=36, voice="kick", level_db=0.0, length_ms=900,
         pitch_hz=50, sweep_from_hz=150, sweep_ms=35, decay_ms=850, attack_ms=0.3,
         click=0.25, click_hz=3000, click_ms=5, drive_db=3, clip="soft"),
    dict(name="sidestick", note=37, voice="rim", level_db=-8.0, length_ms=70,
         tones=((1700, 25, 1.0), (480, 35, 0.6)), click=0.5, click_ms=4, attack_ms=0.1, drive_db=6),
    dict(name="snare", note=38, voice="snare", level_db=-3.0, length_ms=320,
         tones=((185, 140, 1.0), (330, 90, 0.6)), bend=1.15, bend_ms=15,
         snappy=0.8, snappy_low_hz=1500, snappy_high_hz=9000, snappy_ms=220, attack_ms=0.3),
    dict(name="clap", note=39, voice="clap", level_db=-5.0, length_ms=380,
         low_hz=800, high_hz=2200, bursts=4, spacing_ms=10, burst_ms=30, tail=0.6, tail_ms=320),
    # A brighter, noisier, 909-style snare.
    dict(name="snare2", note=40, voice="snare", level_db=-3.0, length_ms=330,
         tones=((205, 110, 1.0), (300, 70, 0.5)), bend=1.5, bend_ms=10,
         snappy=1.3, snappy_low_hz=2500, snappy_high_hz=14000, snappy_ms=260, attack_ms=0.2),
    dict(TOM, name="tom-floor-low", note=41, level_db=-4.0, length_ms=600, pitch_hz=72, decay_ms=560),
    dict(name="hat-closed", note=42, voice="hat", level_db=-10.0, length_ms=110,
         low_hz=7000, high_hz=None, noise=0.2, attack_ms=0.2, decay_ms=70),
    dict(TOM, name="tom-floor-high", note=43, level_db=-4.0, length_ms=560, pitch_hz=86, decay_ms=520),
    dict(name="hat-pedal", note=44, voice="hat", level_db=-12.0, length_ms=180,
         low_hz=6000, high_hz=None, noise=0.25, attack_ms=1.5, decay_ms=130),
    dict(TOM, name="tom-low", note=45, level_db=-4.0, length_ms=520, pitch_hz=102, decay_ms=480),
    dict(name="hat-open", note=46, voice="hat", level_db=-11.0, length_ms=700,
         low_hz=6500, high_hz=None, noise=0.2, attack_ms=0.3, decay_ms=650),
    dict(TOM, name="tom-mid-low", note=47, level_db=-4.0, length_ms=480, pitch_hz=122, decay_ms=440),
    dict(TOM, name="tom-mid-high", note=48, level_db=-4.0, length_ms=440, pitch_hz=145, decay_ms=400),
    dict(name="crash", note=49, voice="cymbal", level_db=-9.0, length_ms=2600,
         low_band=(2800, 6000), low=0.6, low_ms=1300, high_band=(6500, None), high=1.0, high_ms=2500,
         noise=0.5, hit=0.5, hit_ms=90, attack_ms=0.8, bell=0.0, bell_hz=(), bell_ms=1),
    dict(TOM, name="tom-high", note=50, level_db=-4.0, length_ms=400, pitch_hz=172, decay_ms=360),
    dict(name="ride", note=51, voice="cymbal", level_db=-11.0, length_ms=2600,
         low_band=(3500, 8000), low=0.5, low_ms=1800, high_band=(8000, None), high=0.6, high_ms=2400,
         noise=0.1, hit=0.2, hit_ms=40, attack_ms=0.5, bell=0.35, bell_hz=(2550, 3790, 5110), bell_ms=900),
    # The 808's cowbell: two square waves, band-passed, with a quick drop and a longer tail.
    dict(name="cowbell", note=56, voice="cowbell", level_db=-9.0, length_ms=450,
         pitches_hz=(540, 800), low_hz=500, high_hz=2600, attack_ms=0.4,
         fast=0.7, fast_ms=60, slow=0.3, slow_ms=400),
    dict(CONGA, name="conga-high-mute", note=62, level_db=-7.0, length_ms=100, pitch_hz=330, decay_ms=70),
    dict(CONGA, name="conga-high-open", note=63, level_db=-7.0, length_ms=260, pitch_hz=330, decay_ms=230),
    dict(CONGA, name="conga-low", note=64, level_db=-7.0, length_ms=300, pitch_hz=220, decay_ms=270),
    dict(name="maracas", note=70, voice="maracas", level_db=-14.0, length_ms=70,
         low_hz=5000, high_hz=16000, attack_ms=2, decay_ms=50),
    dict(name="claves", note=75, voice="claves", level_db=-10.0, length_ms=80, pitch_hz=2500, decay_ms=60, attack_ms=0.2),
]

# Pieces in a group cut each other off (SPEC §5.2). A kick alone in its group
# cuts itself when struck again, as an 808's does, so a roll doesn't stack up.
CHOKE_GROUPS = [["kick2"], ["kick"], ["hat-closed", "hat-pedal", "hat-open"]]

# ==============================================================================
# Building blocks, at FS.

def times(ms):
    return np.arange(int(round(ms / 1000.0 * FS))) / FS


def fall(t, ms):
    """1 at the start, falling exponentially by 60 dB over ms."""
    return np.exp(-math.log(1000.0) * t / (ms / 1000.0))


def rise(t, ms):
    """0 at the start, rising to 1 over ms in a raised cosine."""
    if ms <= 0:
        return np.ones_like(t)
    x = np.clip(t / (ms / 1000.0), 0.0, 1.0)
    return 0.5 - 0.5 * np.cos(np.pi * x)


def after(t, start_ms):
    """t measured from start_ms, and where it's past that."""
    shifted = t - start_ms / 1000.0
    return np.maximum(shifted, 0.0), shifted >= 0.0


def glide(t, start_hz, end_hz, ms):
    """The phase of a sine whose pitch falls from start_hz towards end_hz, exponentially, with time constant ms."""
    hz = end_hz + (start_hz - end_hz) * np.exp(-t / (ms / 1000.0))
    return 2.0 * np.pi * np.cumsum(hz) / FS


def noise(count, seed):
    return np.random.RandomState(seed).uniform(-1.0, 1.0, count)


def metal(count, pitches_hz=METAL_HZ):
    """Square waves at these pitches, summed, at fixed phases so their edges don't line up."""
    t = np.arange(count) / FS
    total = np.zeros(count)
    for i, hz in enumerate(pitches_hz):
        total += np.sign(np.sin(2.0 * np.pi * hz * t + 0.37 * 2.0 * np.pi * i))
    return total / len(pitches_hz)


def steady(make, count, low_hz=None, high_hz=None, slope=2):
    """A steady sound (noise, oscillators) filtered before it's given an
    envelope: high-passed at low_hz and low-passed at high_hz, with Butterworth
    magnitudes of this order, in the frequency domain. It's made 50 ms longer
    at each end and cut, so the filters' wrap-around never reaches it."""
    pad = int(0.05 * FS)
    x = make(count + 2 * pad)
    spectrum = np.fft.rfft(x)
    hz = np.fft.rfftfreq(len(x), 1.0 / FS)
    gain = np.ones_like(hz)
    if low_hz:
        gain /= np.sqrt(1.0 + (low_hz / np.maximum(hz, 1e-9)) ** (2 * slope))
    if high_hz:
        gain /= np.sqrt(1.0 + (hz / high_hz) ** (2 * slope))
    return np.fft.irfft(spectrum * gain, len(x))[pad:pad + count]


def seed_of(name, purpose):
    return zlib.crc32((name + "/" + purpose).encode("utf-8"))


# ==============================================================================
# The voices: each makes one piece at FS from its parameters.

def kick(p):
    t = times(p["length_ms"])
    body = np.sin(glide(t, p["sweep_from_hz"], p["pitch_hz"], p["sweep_ms"])) * fall(t, p["decay_ms"])
    click = steady(lambda n: noise(n, seed_of(p["name"], "click")), len(t), low_hz=p["click_hz"]) * fall(t, p["click_ms"])
    x = (body + p["click"] * click) * rise(t, p["attack_ms"])
    drive = 10.0 ** (p["drive_db"] / 20.0)

    if p["clip"] == "hard":
        return np.clip(x * drive, -1.0, 1.0)

    return np.tanh(x * drive) / np.tanh(drive)


def snare(p):
    t = times(p["length_ms"])
    tones = sum(level * np.sin(glide(t, hz * p["bend"], hz, p["bend_ms"])) * fall(t, ms) for hz, ms, level in p["tones"])
    snap = steady(lambda n: noise(n, seed_of(p["name"], "snappy")), len(t), p["snappy_low_hz"], p["snappy_high_hz"])
    return (tones + p["snappy"] * snap * fall(t, p["snappy_ms"])) * rise(t, p["attack_ms"])


def rim(p):
    t = times(p["length_ms"])
    tones = sum(level * np.sin(2.0 * np.pi * hz * t) * fall(t, ms) for hz, ms, level in p["tones"])
    click = steady(lambda n: noise(n, seed_of(p["name"], "click")), len(t), low_hz=3000) * fall(t, p["click_ms"])
    x = (tones + p["click"] * click) * rise(t, p["attack_ms"])
    drive = 10.0 ** (p["drive_db"] / 20.0)
    return np.tanh(x * drive) / np.tanh(drive)


def clap(p):
    """Noise, band-passed, struck in quick bursts as the 808 does, then a short tail."""
    t = times(p["length_ms"])
    source = steady(lambda n: noise(n, seed_of(p["name"], "noise")), len(t), p["low_hz"], p["high_hz"])
    envelope = np.zeros_like(t)

    for burst in range(p["bursts"]):
        since, started = after(t, burst * p["spacing_ms"])
        envelope = np.maximum(envelope, np.where(started, fall(since, p["burst_ms"]) * rise(since, 0.3), 0.0))

    since, started = after(t, (p["bursts"] - 1) * p["spacing_ms"])
    envelope = np.maximum(envelope, np.where(started, p["tail"] * fall(since, p["tail_ms"]), 0.0))
    return source * envelope


def hat(p):
    t = times(p["length_ms"])
    seed = seed_of(p["name"], "noise")
    source = steady(lambda n: (1.0 - p["noise"]) * metal(n) + p["noise"] * noise(n, seed), len(t), p["low_hz"], p["high_hz"])
    return source * rise(t, p["attack_ms"]) * fall(t, p["decay_ms"])


def cymbal(p):
    """The 808's metal and noise in two bands, the higher ringing longer, with a burst of noise as it's struck."""
    t = times(p["length_ms"])
    seed = seed_of(p["name"], "noise")
    make = lambda n: (1.0 - p["noise"]) * metal(n) + p["noise"] * noise(n, seed)
    low = steady(make, len(t), *p["low_band"]) * fall(t, p["low_ms"])
    high = steady(make, len(t), *p["high_band"]) * fall(t, p["high_ms"])
    hit = steady(lambda n: noise(n, seed_of(p["name"], "hit")), len(t), low_hz=4000) * fall(t, p["hit_ms"])
    bell = sum(np.sin(2.0 * np.pi * hz * t) for hz in p["bell_hz"]) * fall(t, p["bell_ms"]) if p["bell_hz"] else 0.0
    return (p["low"] * low + p["high"] * high + p["hit"] * hit + p["bell"] * bell) * rise(t, p["attack_ms"])


def cowbell(p):
    t = times(p["length_ms"])
    source = steady(lambda n: metal(n, p["pitches_hz"]), len(t), p["low_hz"], p["high_hz"])
    envelope = p["fast"] * fall(t, p["fast_ms"]) + p["slow"] * fall(t, p["slow_ms"])
    return source * envelope * rise(t, p["attack_ms"])


def tom(p):
    """A sine falling a little in pitch, with a touch of noise: the 808's toms and congas."""
    t = times(p["length_ms"])
    body = np.sin(glide(t, p["pitch_hz"] * p["bend"], p["pitch_hz"], p["bend_ms"])) * fall(t, p["decay_ms"])
    hiss = steady(lambda n: noise(n, seed_of(p["name"], "noise")), len(t), 300, 3000) * fall(t, p["noise_ms"])
    return (body + p["noise"] * hiss) * rise(t, p["attack_ms"])


def maracas(p):
    t = times(p["length_ms"])
    source = steady(lambda n: noise(n, seed_of(p["name"], "noise")), len(t), p["low_hz"], p["high_hz"])
    return source * rise(t, p["attack_ms"]) * fall(t, p["decay_ms"])


def claves(p):
    t = times(p["length_ms"])
    return np.sin(2.0 * np.pi * p["pitch_hz"] * t) * fall(t, p["decay_ms"]) * rise(t, p["attack_ms"])


VOICES = dict(kick=kick, snare=snare, rim=rim, clap=clap, hat=hat, cymbal=cymbal, cowbell=cowbell, tom=tom,
              maracas=maracas, claves=claves)

# ==============================================================================
# From FS down to the samples.

def lowpass_taps(count=257):
    """A linear-phase low-pass at FS whose cutoff is midway between 20 kHz, which
    it keeps, and 24.1 kHz, above which anything would fold back below 20 kHz
    at 44.1 kHz. A Kaiser window puts what it stops 80 dB down."""
    cutoff = 22050.0 / FS
    m = np.arange(count) - (count - 1) / 2.0
    taps = 2.0 * cutoff * np.sinc(2.0 * cutoff * m) * np.kaiser(count, 8.0)
    return taps / taps.sum()


TAPS = lowpass_taps()


def to_rate(x):
    """Filtered and taken down to RATE, delayed by nothing: the filter's delay is
    taken off, and what it rings before the start is dropped (every piece rises
    from silence, so that's next to nothing)."""
    delay = (len(TAPS) - 1) // 2
    return np.convolve(x, TAPS)[delay:delay + len(x)][::OVERSAMPLE]


def finish(x, name):
    """The piece's 16-bit samples: at RATE, peaking at SAMPLE_PEAK_DBFS, its last
    FADE_MS faded to silence, dithered with the dither faded too, so it ends at 0.
    Its first FADE_IN samples fade in, so it starts at 0 too, whatever the
    filter rang before a steep start (a hard clip's)."""
    y = to_rate(x)
    y *= 10.0 ** (SAMPLE_PEAK_DBFS / 20.0) / np.max(np.abs(y))
    fade = int(round(FADE_MS / 1000.0 * RATE))
    window = np.ones(len(y))
    window[-fade:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(1, fade + 1) / fade)
    window[:FADE_IN] = 0.5 - 0.5 * np.cos(np.pi * np.arange(FADE_IN) / FADE_IN)
    rng = np.random.RandomState(seed_of(name, "dither"))
    dither = (rng.uniform(-0.5, 0.5, len(y)) + rng.uniform(-0.5, 0.5, len(y))) * window
    return np.clip(np.round(y * window * 32768.0 + dither), -32768, 32767).astype("<i2")


def true_peak(x):
    """The peak between samples too: x upsampled 4 times in the frequency domain."""
    n = len(x)
    spectrum = np.fft.rfft(x, 2 * n)  # padded, so the end doesn't wrap into the start
    up = np.fft.irfft(spectrum, 8 * n) * 4.0
    return np.max(np.abs(up))


def db(gain):
    return 20.0 * math.log10(gain)


def kit_levels(samples):
    """Each piece's level in kit.json, in steps of 0.1 dB: its level_db plus the
    kit's trim, so that every piece struck at once at v127 peaks at KIT_PEAK_DBFS
    or under, between samples too. Also that peak, in dBFS."""
    length = max(len(s) for s in samples.values())

    def peak(levels):
        mix = np.zeros(length)
        for piece in PIECES:
            s = samples[piece["name"]].astype(np.float64) / 32768.0
            mix[:len(s)] += s * 10.0 ** (levels[piece["name"]] / 20.0)
        return db(true_peak(mix))

    trim = KIT_PEAK_DBFS - peak({p["name"]: p["level_db"] for p in PIECES})
    levels = {p["name"]: math.floor((p["level_db"] + trim) * 10.0 + 1e-9) / 10.0 for p in PIECES}

    while peak(levels) > KIT_PEAK_DBFS:
        levels = {name: round(level - 0.1, 1) for name, level in levels.items()}

    return levels, peak(levels)


# ==============================================================================
# The files.

def wav_bytes(samples):
    data = samples.astype("<i2").tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    return header + b"data" + struct.pack("<I", len(data)) + data


def level_text(level):
    """As Hocket writes a volume: "-12.3dB", "-12dB", "0dB"."""
    text = ("%.1f" % level).rstrip("0").rstrip(".")
    return ("0" if text in ("-0", "0") else text) + "dB"


def manifest_text(levels):
    lines = ["{", '  "format": "hocket-kit",', '  "formatVersion": 0,', '  "description": "%s",' % DESCRIPTION, '  "pieces": [']
    for i, piece in enumerate(PIECES):
        lines.append('    { "name": "%s", "note": %d, "file": "%s.wav", "level": "%s" }%s'
                     % (piece["name"], piece["note"], piece["name"], level_text(levels[piece["name"]]), "," if i + 1 < len(PIECES) else ""))
    lines.append("  ],")
    groups = ", ".join("[" + ", ".join('"%s"' % name for name in group) + "]" for group in CHOKE_GROUPS)
    lines.append('  "chokeGroups": [%s]' % groups)
    lines.append("}")
    return ("\n".join(lines) + "\n").encode("utf-8")


def make_kit():
    """Every file of the kit, by name, and what to say about it."""
    samples = {}
    for piece in PIECES:
        samples[piece["name"]] = finish(VOICES[piece["voice"]](piece), piece["name"])

    levels, kit_peak = kit_levels(samples)
    files = {piece["name"] + ".wav": wav_bytes(samples[piece["name"]]) for piece in PIECES}
    files["kit.json"] = manifest_text(levels)
    files["LICENSE.txt"] = LICENSE.encode("utf-8")

    report = []
    for piece in PIECES:
        s = samples[piece["name"]]
        level = levels[piece["name"]]
        report.append("  %-16s %3d  %5d ms  level %7s  alone at v127: %6.1f dBFS"
                      % (piece["name"], piece["note"], round(len(s) * 1000 / RATE), level_text(level),
                         db(true_peak(s.astype(np.float64) / 32768.0)) + level))
    seconds = sum(len(s) for s in samples.values()) / RATE
    size = sum(len(data) for name, data in files.items() if name.endswith(".wav"))
    report.append("%d pieces, %.1f s of audio, %.2f MB of WAVs; all struck at once at v127: %.2f dBFS"
                  % (len(PIECES), seconds, size / 1e6, kit_peak))
    return files, report


def main():
    parser = argparse.ArgumentParser(description="Synthesizes Hocket's electronic drum kit into " + KIT_FOLDER)
    parser.add_argument("--check", action="store_true", help="say whether the kit on disk matches, and write nothing")
    args = parser.parse_args()

    started = time.time()
    files, report = make_kit()
    stale = sorted(name for name in (os.listdir(KIT_FOLDER) if os.path.isdir(KIT_FOLDER) else [])
                   if name.endswith(".wav") and name not in files)

    if args.check:
        differ = []
        for name, data in sorted(files.items()):
            path = os.path.join(KIT_FOLDER, name)
            if not os.path.exists(path) or open(path, "rb").read() != data:
                differ.append(name)
        for name in differ:
            print("differs: " + name)
        for name in stale:
            print("not part of the kit: " + name)
        if differ or stale:
            return 1
        print("The kit in %s matches the generator (%.1f s)." % (KIT_FOLDER, time.time() - started))
        return 0

    os.makedirs(KIT_FOLDER, exist_ok=True)
    for name, data in sorted(files.items()):
        with open(os.path.join(KIT_FOLDER, name), "wb") as f:
            f.write(data)

    print("\n".join(report))
    print("Wrote %d files to %s (%.1f s)." % (len(files), KIT_FOLDER, time.time() - started))
    for name in stale:
        print("Not part of the kit any more, so delete it: " + name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
