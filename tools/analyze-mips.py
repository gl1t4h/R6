#!/usr/bin/env python3
"""Perform a conservative, metadata-only first pass over a local PS-X EXE.

This is not a full disassembler. It identifies instruction classes, direct
JAL targets, and likely function return sites so the next recompiler pass has
repeatable input. The generated report should be kept outside Git.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def signed(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe_path", type=Path, help="local PS-X EXE analysis copy")
    args = parser.parse_args()

    exe = args.exe_path.resolve()
    data = exe.read_bytes()
    if data[:8] != b"PS-X EXE":
        parser.error("input does not have a PS-X EXE header")

    load_address = word(data, 0x18)
    payload_size = word(data, 0x1C)
    initial_pc = word(data, 0x10)
    payload = data[2048 : 2048 + payload_size]
    if len(payload) != payload_size:
        parser.error("PS-X EXE payload is shorter than its header declares")

    instruction_count = len(payload) // 4
    classes: dict[str, int] = {}
    jal_targets: set[int] = set()
    return_sites: set[int] = set()
    branch_count = 0

    for index in range(instruction_count):
        pc = load_address + index * 4
        instruction = word(payload, index * 4)
        opcode = instruction >> 26

        if opcode == 0:
            funct = instruction & 0x3F
            if funct == 0x08:
                classes["jr"] = classes.get("jr", 0) + 1
                if ((instruction >> 21) & 0x1F) == 31:
                    return_sites.add(pc)
            elif funct == 0x09:
                classes["jalr"] = classes.get("jalr", 0) + 1
            else:
                classes["special"] = classes.get("special", 0) + 1
        elif opcode == 0x03:
            classes["jal"] = classes.get("jal", 0) + 1
            target = ((pc + 4) & 0xF0000000) | ((instruction & 0x03FFFFFF) << 2)
            if load_address <= target < load_address + payload_size:
                jal_targets.add(target)
        elif opcode == 0x02:
            classes["j"] = classes.get("j", 0) + 1
        elif opcode in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
            classes["branch"] = classes.get("branch", 0) + 1
            branch_count += 1
        elif opcode in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2A, 0x2B):
            classes["memory"] = classes.get("memory", 0) + 1
        elif opcode == 0x0F:
            classes["lui"] = classes.get("lui", 0) + 1
        elif opcode in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F):
            classes["immediate"] = classes.get("immediate", 0) + 1
        else:
            classes["other"] = classes.get("other", 0) + 1

    functions = sorted({initial_pc, *jal_targets})
    report = {
        "input_bytes": len(data),
        "load_address": f"0x{load_address:08X}",
        "payload_bytes": payload_size,
        "initial_pc": f"0x{initial_pc:08X}",
        "instruction_count": instruction_count,
        "instruction_classes": classes,
        "branch_count": branch_count,
        "direct_call_target_count": len(jal_targets),
        "likely_function_return_count": len(return_sites),
        "function_candidates": [f"0x{address:08X}" for address in functions],
        "note": "Conservative first-pass metadata; no translated code or game assets are emitted.",
    }
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
