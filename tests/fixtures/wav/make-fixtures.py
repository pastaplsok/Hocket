"""Writes the WAV fixtures beside this script: small files laid out the way
real sample packs lay them out, for the kit header reader (tests/core) and
the decoder (tests/engine). Run it again only to change them:

    python tests/fixtures/wav/make-fixtures.py

Standard library only. The output is the same every run.

Every file holds 32 frames. Channel 1's frame i is (i + 1) / 64, from 1/64
to 0.5; channel 2, where there is one, is its negative. "float32-loud.wav"
is three times that, peaking at 1.5, above full scale. The tests work the
values out the same way.
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = 32

PCM = 1
FLOAT = 3
EXTENSIBLE = 0xFFFE
GUID_TAIL = bytes([0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])


def value(frame, channel, scale=1.0):
    v = (frame + 1) / 64 * scale
    return v if channel == 0 else -v


def chunk(name, body, pad=True):
    data = name.encode("ascii") + struct.pack("<I", len(body)) + body
    if pad and len(body) % 2 == 1:
        data += b"\0"
    return data


def fmt_plain(tag, channels, rate, bits, extra=None):
    block = channels * bits // 8
    body = struct.pack("<HHIIHH", tag, channels, rate, rate * block, block, bits)
    if extra is not None:
        body += struct.pack("<H", len(extra)) + extra
    return chunk("fmt ", body)


def fmt_extensible(subtag, channels, rate, container, valid):
    block = channels * container // 8
    mask = 0x4 if channels == 1 else 0x3
    extra = struct.pack("<HI", valid, mask) + struct.pack("<H", subtag) + GUID_TAIL
    body = struct.pack("<HHIIHH", EXTENSIBLE, channels, rate, rate * block, block, container) + struct.pack("<H", len(extra)) + extra
    return chunk("fmt ", body)


def samples(encoding, channels, container, valid=None, scale=1.0):
    valid = valid or container
    out = bytearray()
    for frame in range(FRAMES):
        for channel in range(channels):
            v = value(frame, channel, scale)
            if encoding == "float":
                out += struct.pack("<f", v)
            elif valid == 8:
                out += struct.pack("<B", 128 + round(v * 127))
            else:
                full = (1 << (valid - 1)) - 1
                n = round(v * full) << (container - valid)
                out += n.to_bytes(container // 8, "little", signed=True)
    return bytes(out)


def riff(chunks, form=b"RIFF", size=None):
    body = b"WAVE" + b"".join(chunks)
    return form + struct.pack("<I", len(body) if size is None else size) + body


def write(name, data):
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(data)


def fact():
    return chunk("fact", struct.pack("<I", FRAMES))


# The plain layouts.
write("pcm16-mono.wav", riff([fmt_plain(PCM, 1, 44100, 16), chunk("data", samples("pcm", 1, 16))]))
write("pcm24-stereo.wav", riff([fmt_plain(PCM, 2, 44100, 24), chunk("data", samples("pcm", 2, 24))]))
write("float32-stereo.wav", riff([fmt_plain(FLOAT, 2, 44100, 32, extra=b""), fact(), chunk("data", samples("float", 2, 32))]))
write("float32-loud.wav", riff([fmt_plain(FLOAT, 1, 44100, 32, extra=b""), fact(), chunk("data", samples("float", 1, 32, scale=3.0))]))

# The extensible format header (WAVE_FORMAT_EXTENSIBLE), as many tools write
# for 24-bit, float and anything over two channels.
write("extensible-pcm24.wav", riff([fmt_extensible(PCM, 2, 44100, 24, 24), chunk("data", samples("pcm", 2, 24))]))
write("extensible-pcm24-in-32.wav", riff([fmt_extensible(PCM, 1, 44100, 32, 24), chunk("data", samples("pcm", 1, 32, valid=24))]))
write("extensible-float32-48k.wav", riff([fmt_extensible(FLOAT, 2, 48000, 32, 32), fact(), chunk("data", samples("float", 2, 32))]))

# Other chunks before and after the audio, odd-length ones with their pad
# byte, and a last odd chunk whose pad byte is missing (allowed at the end).
bext = struct.pack("<256s32s32s10s8sQH64s190s", b"Kick, dry", b"Hocket", b"fixture", b"2026-11-17", b"12:00:00", 0, 1, b"", b"")
info = b"INFO" + chunk("INAM", b"Kick")  # 4 bytes, even
info_odd = b"INFO" + chunk("ICMT", b"odd", pad=False)  # 15 bytes in all: the LIST is odd-length
smpl = struct.pack("<9I", 0, 0, 22676, 60, 0, 0, 0, 1, 0) + struct.pack("<6I", 0, 0, 0, FRAMES - 1, 0, 0)
acid = struct.pack("<IHHfIHHf", 0x1, 60, 0x8000, 0.0, 1, 4, 4, 120.0)
write(
    "chunks-around.wav",
    riff([
        chunk("JUNK", bytes(28)),
        chunk("bext", bext),
        fmt_plain(PCM, 2, 44100, 16),
        chunk("LIST", info),
        chunk("LIST", info_odd),  # an odd-length chunk, then its pad byte
        chunk("data", samples("pcm", 2, 16)),
        chunk("smpl", smpl),
        chunk("acid", acid),
        chunk("zzzz", b"end", pad=False),  # the file ends without this pad byte
    ]),
)

# What a kit can't use, for the header reader to refuse.
write("pcm8-mono.wav", riff([fmt_plain(PCM, 1, 44100, 8), chunk("data", samples("pcm", 1, 8))]))
write("three-channels.wav", riff([fmt_extensible(PCM, 3, 44100, 16, 16), chunk("data", samples("pcm", 3, 16))]))

# What the header reader can't make sense of, so the decoder decides: an RF64
# file (64-bit sizes, which the decoder reads), and a data chunk whose size
# was never filled in (which it refuses).
pcm_mono = samples("pcm", 1, 16)
ds64 = struct.pack("<QQQI", 4 + 36 + 24 + 8 + len(pcm_mono), len(pcm_mono), FRAMES, 0)
write(
    "rf64-pcm16-mono.wav",
    riff([chunk("ds64", ds64), fmt_plain(PCM, 1, 44100, 16), b"data" + struct.pack("<I", 0xFFFFFFFF) + pcm_mono], form=b"RF64", size=0xFFFFFFFF),
)
write(
    "data-size-unknown.wav",
    riff([fmt_plain(PCM, 1, 44100, 16), b"data" + struct.pack("<I", 0xFFFFFFFF) + pcm_mono]),
)
