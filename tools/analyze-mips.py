#!/usr/bin/env python3
"""Build a conservative control-flow summary for a local PS-X EXE.

This tool emits metadata only. It does not write translated code, game data,
or a disassembly into the repository.
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import deque
from pathlib import Path

from mips_decode import decode_word


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def in_image(address: int, load: int, size: int) -> bool:
    return load <= address < load + size and (address - load) % 4 == 0


def analyze_function(
    entry: int,
    payload: bytes,
    load: int,
    size: int,
    max_instructions: int,
) -> tuple[dict, set[int]]:
    pending = deque([entry])
    visited: set[int] = set()
    blocks: dict[int, dict] = {}
    discovered_calls: set[int] = set()
    returns = 0
    indirect_calls = 0

    def read_instruction(address: int) -> int:
        return word(payload, address - load)

    while pending and len(visited) < max_instructions:
        block_start = pending.popleft()
        if block_start in blocks or not in_image(block_start, load, size):
            continue

        pc = block_start
        block_instructions: list[int] = []
        edges: set[int] = set()
        terminated = False

        while (
            in_image(pc, load, size)
            and pc not in visited
            and len(visited) < max_instructions
        ):
            visited.add(pc)
            block_instructions.append(pc)
            decoded = decode_word(read_instruction(pc), pc)

            if decoded["kind"] == "call":
                target = decoded["target"]
                if in_image(target, load, size):
                    discovered_calls.add(target)
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                    block_instructions.append(delay)
                continuation = pc + 8
                if in_image(continuation, load, size):
                    edges.add(continuation)
                    pending.append(continuation)
                terminated = True
                break

            if decoded["kind"] == "indirect_call":
                indirect_calls += 1
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                    block_instructions.append(delay)
                continuation = pc + 8
                if in_image(continuation, load, size):
                    edges.add(continuation)
                    pending.append(continuation)
                terminated = True
                break

            if decoded["kind"] == "return":
                returns += 1
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                    block_instructions.append(delay)
                terminated = True
                break

            if decoded["kind"] == "jump":
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                    block_instructions.append(delay)
                target = decoded["target"]
                if in_image(target, load, size):
                    edges.add(target)
                    pending.append(target)
                terminated = True
                break

            if decoded["kind"] == "branch":
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                    block_instructions.append(delay)
                target = decoded["target"]
                continuation = pc + 8
                for successor in (target, continuation):
                    if in_image(successor, load, size):
                        edges.add(successor)
                        pending.append(successor)
                terminated = True
                break

            if decoded["kind"] == "trap":
                terminated = True
                break

            pc += 4

        if block_instructions:
            blocks[block_start] = {
                "address": f"0x{block_start:08X}",
                "instructions": len(block_instructions),
                "last_address": f"0x{block_instructions[-1]:08X}",
                "successors": [f"0x{address:08X}" for address in sorted(edges)],
                "terminated": terminated,
            }

    return (
        {
            "address": f"0x{entry:08X}",
            "instructions": sum(block["instructions"] for block in blocks.values()),
            "blocks": len(blocks),
            "returns": returns,
            "indirect_calls": indirect_calls,
            "direct_calls": len(discovered_calls),
            "block_details": list(blocks.values()),
            "truncated": len(visited) >= max_instructions,
        },
        discovered_calls,
    )


def direct_call_targets(payload: bytes, load: int, size: int) -> set[int]:
    targets = set()
    for offset in range(0, len(payload) - 3, 4):
        pc = load + offset
        instruction = word(payload, offset)
        if instruction >> 26 == 0x03:
            target = ((pc + 4) & 0xF0000000) | ((instruction & 0x03FFFFFF) << 2)
            if in_image(target, load, size):
                targets.add(target)
    return targets


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe_path", type=Path, help="local PS-X EXE analysis copy")
    parser.add_argument(
        "--max-functions",
        type=int,
        default=32,
        help="safety limit for discovered functions",
    )
    parser.add_argument(
        "--max-instructions-per-function",
        type=int,
        default=5000,
        help="safety limit for each control-flow walk",
    )
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

    functions: list[dict] = []
    seen: set[int] = set()
    seeded_calls = direct_call_targets(payload, load_address, payload_size)
    pending = deque([initial_pc, *sorted(seeded_calls)])
    while pending and len(functions) < args.max_functions:
        entry = pending.popleft()
        if entry in seen or not in_image(entry, load_address, payload_size):
            continue
        seen.add(entry)
        function, calls = analyze_function(
            entry,
            payload,
            load_address,
            payload_size,
            args.max_instructions_per_function,
        )
        functions.append(function)
        for target in sorted(calls):
            if target not in seen:
                pending.append(target)

    report = {
        "load_address": f"0x{load_address:08X}",
        "payload_bytes": payload_size,
        "initial_pc": f"0x{initial_pc:08X}",
        "static_direct_call_seed_count": len(seeded_calls),
        "reachable_function_count": len(functions),
        "unprocessed_function_count": len(pending),
        "reachable_instruction_count": sum(
            function["instructions"] for function in functions
        ),
        "functions": functions,
        "note": (
            "Conservative CFG metadata only. Delay slots are accounted for; "
            "indirect control flow and data/code boundaries require later analysis."
        ),
    }
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
