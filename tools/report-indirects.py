#!/usr/bin/env python3
"""Report potential indirect MIPS control-flow sites in a local PS-X EXE."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from mips_decode import decode_word


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe_path", type=Path, help="local PS-X EXE analysis copy")
    parser.add_argument("--max-sites", type=int, default=256)
    args = parser.parse_args()

    exe = args.exe_path.resolve()
    data = exe.read_bytes()
    if data[:8] != b"PS-X EXE":
        parser.error("input does not have a PS-X EXE header")

    load = word(data, 0x18)
    payload_size = word(data, 0x1C)
    payload = data[2048 : 2048 + payload_size]
    sites = []

    for offset in range(0, len(payload) - 3, 4):
        address = load + offset
        decoded = decode_word(word(payload, offset), address)
        if decoded["kind"] not in ("jump_register", "indirect_call"):
            continue
        sites.append({
            "address": f"0x{address:08X}",
            "kind": decoded["kind"],
            "text": decoded["text"],
            "status": "requires_contextual_resolution",
        })

    report = {
        "potential_indirect_site_count": len(sites),
        "reported_sites": sites[: args.max_sites],
        "truncated": len(sites) > args.max_sites,
        "note": (
            "Static candidates only. Sites may be data interpreted as code; "
            "the CFG pass must validate reachability."
        ),
    }
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
