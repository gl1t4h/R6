#!/usr/bin/env python3
"""Generate the synthetic CHD fixtures used by tests/test_chd_image.cpp.

The input images are built here from repeated ASCII text, so nothing in this
directory is game data and no disc is needed.  `chdman` (MAME 0.289 was used
to produce the committed files) is the only external tool:

    python3 tests/make_chd_fixtures.py                # chdman from PATH
    CHDMAN=/path/to/chdman python3 tests/make_chd_fixtures.py

Every fixture is a 40-sector Mode 2 image (8 sectors per hunk by default):

    synthetic-none.chd       uncompressed hunks, 4-byte map
    synthetic-cdzl.chd       DEFLATE hunks, compressed map
    synthetic-cdlz.chd       LZMA hunks, compressed map
    synthetic-default.chd    chdman's default codec set
    synthetic-duplicate.chd  repeated hunks, exercising SELF references
    synthetic-sparse.chd     zero-heavy sectors, exercising LZMA rep distances
    synthetic-small-hunk.chd two-sector hunks, exercising the distance tree
    synthetic-large-hunk.chd 32-sector hunks, 3-byte compressed lengths
    synthetic-audio.chd      data track followed by an audio track
    synthetic-zstd.chd       Zstandard hunks, which R6 rejects by design
"""

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTORS = 40
SECTOR_BYTES = 2352
USER_BYTES = 2048
ORDER = Path(__file__).resolve().parent / "fixtures"
CHDMAN = os.environ.get("CHDMAN", "chdman")


def bcd(value):
    return ((value // 10) << 4) | (value % 10)


def msf(lba):
    frames = lba + 150
    return bytes([bcd(frames // 4500), bcd((frames // 75) % 60),
                  bcd(frames % 75)])


def text_body(phrase, index):
    text = (phrase.format(index=index) * 128).encode("ascii")
    return text[:USER_BYTES]


def sector(lba, body):
    sync = b"\x00" + b"\xff" * 10 + b"\x00"
    subheader = bytes([0, 0, 0x08, 0]) * 2
    raw = sync + msf(lba) + b"\x02" + subheader + body
    return raw + bytes(SECTOR_BYTES - len(raw))


def write_image(path, phrase):
    data = bytearray()
    for lba in range(SECTORS):
        data += sector(lba, text_body(phrase, lba))
    path.write_bytes(bytes(data))


def write_duplicate_image(path):
    """Hunks 1/3/4 repeat earlier hunks so chdman stores SELF entries."""
    data = bytearray()
    for lba in range(SECTORS):
        block = "a" if (lba % 16) < 8 else "b"
        phrase = f"r6 duplicate block {block} ".encode("ascii")
        body = (phrase * 128)[:USER_BYTES]
        data += sector(lba, body)
    path.write_bytes(bytes(data))


def write_sparse_image(path):
    """Mostly zero sectors, so LZMA emits long matches and rep rotations."""
    data = bytearray()
    for lba in range(SECTORS):
        body = bytearray(USER_BYTES)
        if lba in (0, 7, 20, 33):
            phrase = f"r6 sparse sector {lba:04d} ".encode("ascii") * 8
            body[:64] = phrase[:64]
        if lba == 39:
            body[2000:2011] = b"tail marker"
        data += sector(lba, bytes(body))
    path.write_bytes(bytes(data))


def write_audio_track(path):
    frames = 10
    data = bytearray()
    for frame in range(frames):
        for sample in range(588):
            value = int(12000 * math.sin(2 * math.pi * 440 *
                                        (frame * 588 + sample) / 44100))
            data += struct.pack("<h", value) * 2
    path.write_bytes(bytes(data))


def run_chdman(*arguments):
    subprocess.run([CHDMAN, *arguments], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main():
    if shutil.which(CHDMAN) is None:
        sys.exit(f"chdman not found (set CHDMAN): {CHDMAN}")
    ORDER.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as workspace:
        work = Path(workspace)
        data = work / "data.bin"
        write_image(data, "r6 synthetic sector {index:04d} ")
        duplicate = work / "duplicate.bin"
        write_duplicate_image(duplicate)
        sparse = work / "sparse.bin"
        write_sparse_image(sparse)
        audio = work / "audio.bin"
        write_audio_track(audio)

        cue = work / "disc.cue"
        cue.write_text('FILE "data.bin" BINARY\n'
                       '  TRACK 01 MODE2/2352\n'
                       '    INDEX 01 00:00:00\n', encoding="ascii")
        duplicate_cue = work / "duplicate.cue"
        duplicate_cue.write_text('FILE "duplicate.bin" BINARY\n'
                                 '  TRACK 01 MODE2/2352\n'
                                 '    INDEX 01 00:00:00\n', encoding="ascii")
        sparse_cue = work / "sparse.cue"
        sparse_cue.write_text('FILE "sparse.bin" BINARY\n'
                              '  TRACK 01 MODE2/2352\n'
                              '    INDEX 01 00:00:00\n', encoding="ascii")
        audio_cue = work / "audio.cue"
        audio_cue.write_text('FILE "data.bin" BINARY\n'
                             '  TRACK 01 MODE2/2352\n'
                             '    INDEX 01 00:00:00\n'
                             'FILE "audio.bin" BINARY\n'
                             '  TRACK 02 AUDIO\n'
                             '    INDEX 01 00:00:00\n', encoding="ascii")

        recipes = [
            ("synthetic-none.chd", cue, ["-c", "none"]),
            ("synthetic-cdzl.chd", cue, ["-c", "cdzl"]),
            ("synthetic-cdlz.chd", cue, ["-c", "cdlz"]),
            ("synthetic-default.chd", cue, []),
            ("synthetic-duplicate.chd", duplicate_cue, []),
            ("synthetic-sparse.chd", sparse_cue, ["-c", "cdlz"]),
            ("synthetic-small-hunk.chd", cue,
             ["-hs", "4896", "-c", "cdlz"]),
            ("synthetic-large-hunk.chd", cue, ["-hs", "78336"]),
            ("synthetic-audio.chd", audio_cue, []),
            ("synthetic-zstd.chd", cue, ["-c", "cdzs"]),
        ]
        for name, source, options in recipes:
            target = ORDER / name
            if target.exists():
                target.unlink()
            run_chdman("createcd", "-i", str(source), "-o", str(target),
                       "-f", *options)
            print(f"{name}: {target.stat().st_size} bytes")


if __name__ == "__main__":
    main()
