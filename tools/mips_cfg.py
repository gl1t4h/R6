"""Conservative MIPS control-flow discovery for local analysis artifacts."""

from __future__ import annotations

from collections import deque
import struct

from mips_decode import decode_word


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def in_image(address: int, load: int, size: int) -> bool:
    return load <= address < load + size and (address - load) % 4 == 0


def walk_function(
    payload: bytes,
    load: int,
    size: int,
    entry: int,
    max_instructions: int,
) -> tuple[set[int], set[int]]:
    pending = deque([entry])
    visited: set[int] = set()
    calls: set[int] = set()

    while pending and len(visited) < max_instructions:
        block = pending.popleft()
        if not in_image(block, load, size):
            continue
        pc = block
        while in_image(pc, load, size) and pc not in visited:
            if len(visited) >= max_instructions:
                break
            visited.add(pc)
            instruction = word(payload, pc - load)
            decoded = decode_word(instruction, pc)
            kind = decoded["kind"]

            if kind == "call":
                target = decoded["target"]
                if in_image(target, load, size):
                    calls.add(target)
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                continuation = pc + 8
                if in_image(continuation, load, size):
                    pending.append(continuation)
                break
            if kind == "indirect_call":
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                continuation = pc + 8
                if in_image(continuation, load, size):
                    pending.append(continuation)
                break
            if kind == "return" or kind == "trap":
                delay = pc + 4
                if kind == "return" and in_image(delay, load, size):
                    visited.add(delay)
                break
            if kind == "jump":
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                target = decoded["target"]
                if in_image(target, load, size):
                    pending.append(target)
                break
            if kind == "branch":
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                for successor in (decoded["target"], pc + 8):
                    if in_image(successor, load, size):
                        pending.append(successor)
                break
            pc += 4

    return visited, calls


def discover_reachable_functions(
    payload: bytes,
    load: int,
    initial_pc: int,
    max_functions: int = 256,
    max_instructions_per_function: int = 20000,
) -> list[int]:
    """Discover functions reachable through direct in-image calls.

    Indirect calls, jump tables, and data/code ambiguity are intentionally not
    guessed. Results are therefore conservative and may be incomplete.
    """
    pending = deque([initial_pc])
    discovered: list[int] = []
    seen: set[int] = set()
    while pending and len(discovered) < max_functions:
        entry = pending.popleft()
        if entry in seen or not in_image(entry, load, len(payload)):
            continue
        seen.add(entry)
        discovered.append(entry)
        _, calls = walk_function(
            payload, load, len(payload), entry, max_instructions_per_function
        )
        for target in sorted(calls):
            if target not in seen:
                pending.append(target)
    return discovered
