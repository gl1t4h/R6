#!/usr/bin/env python3
"""Inspect a local PlayStation 1 BIN/CUE without adding game data to the repo.

The default operation reads only filesystem and PS-X EXE header metadata.
Use --extract-exe only when a local analysis copy is needed, and write it
outside the repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

RAW_SECTOR_SIZE = 2352
USER_DATA_OFFSET = 24
USER_DATA_SIZE = 2048


def read_user_sector(handle, sector: int) -> bytes:
    handle.seek(sector * RAW_SECTOR_SIZE + USER_DATA_OFFSET)
    data = handle.read(USER_DATA_SIZE)
    if len(data) != USER_DATA_SIZE:
        raise ValueError(f"short sector read at raw sector {sector}")
    return data


def u32le(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_record(data: bytes, offset: int) -> dict:
    length = data[offset]
    if length < 34 or offset + length > len(data):
        raise ValueError(f"invalid ISO9660 directory record at {offset}")
    record = data[offset : offset + length]
    name_length = record[32]
    name = record[33 : 33 + name_length].decode("ascii", errors="replace")
    return {
        "name": name,
        "extent": u32le(record, 2),
        "size": u32le(record, 10),
        "directory": bool(record[25] & 0x02),
        "length": length,
    }


def root_entries(handle, root_extent: int, root_size: int) -> list[dict]:
    result = []
    remaining = root_size
    sector = root_extent
    while remaining:
        block = read_user_sector(handle, sector)
        usable = min(remaining, USER_DATA_SIZE)
        offset = 0
        while offset < usable:
            length = block[offset]
            if length == 0:
                break
            entry = parse_record(block, offset)
            if entry["name"] not in ("\x00", "\x01"):
                result.append(entry)
            offset += entry["length"]
        sector += 1
        remaining -= usable
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bin_path", type=Path, help="local raw BIN file")
    parser.add_argument(
        "--extract-exe",
        type=Path,
        help="optional output path outside the repository for local analysis",
    )
    args = parser.parse_args()

    disc = args.bin_path.resolve()
    if not disc.is_file():
        parser.error(f"BIN file does not exist: {disc}")

    with disc.open("rb") as handle:
        pvd = read_user_sector(handle, 16)
        if pvd[1:6] != b"CD001":
            raise ValueError("ISO9660 primary volume descriptor not found")

        root = parse_record(pvd, 156)
        entries = root_entries(handle, root["extent"], root["size"])
        by_name = {entry["name"].split(";", 1)[0].upper(): entry for entry in entries}

        executable = by_name.get("SLUS_008.75")
        if executable is None:
            executable = next(
                (
                    entry
                    for entry in entries
                    if entry["name"].upper().startswith(("SLUS_", "SCUS_", "SLES_"))
                ),
                None,
            )
        if executable is None:
            raise ValueError("no root-level PlayStation executable was found")

        exe_offset = executable["extent"] * RAW_SECTOR_SIZE + USER_DATA_OFFSET
        handle.seek(exe_offset)
        header = handle.read(2048)
        if header[:8] != b"PS-X EXE":
            raise ValueError("disc executable does not have a PS-X EXE header")

        metadata = {
            "disc_sha256": sha256_file(disc),
            "disc_bytes": disc.stat().st_size,
            "filesystem": "ISO9660 on raw Mode2/2352 sectors",
            "root_entries": [
                {
                    "name": entry["name"],
                    "extent": entry["extent"],
                    "bytes": entry["size"],
                    "directory": entry["directory"],
                }
                for entry in entries
            ],
            "executable": {
                "name": executable["name"],
                "extent": executable["extent"],
                "bytes": executable["size"],
                "sha256": None,
                "psx_exe": {
                    "initial_pc": f"0x{u32le(header, 0x10):08X}",
                    "initial_gp": f"0x{u32le(header, 0x14):08X}",
                    "load_address": f"0x{u32le(header, 0x18):08X}",
                    "payload_bytes": u32le(header, 0x1C),
                    "memfill_start": f"0x{u32le(header, 0x30):08X}",
                    "memfill_size": u32le(header, 0x34),
                },
            },
            "note": "Metadata only; no game assets are included in this report.",
        }

        if args.extract_exe:
            output = args.extract_exe.resolve()
            output.parent.mkdir(parents=True, exist_ok=True)
            handle.seek(exe_offset)
            payload = handle.read(executable["size"])
            output.write_bytes(payload)
            metadata["executable"]["sha256"] = hashlib.sha256(payload).hexdigest().upper()
            metadata["executable"]["local_analysis_copy"] = str(output)

    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
