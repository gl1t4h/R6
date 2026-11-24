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
    pending = deque([(entry, {})])
    visited: set[int] = set()
    calls: set[int] = set()

    while pending and len(visited) < max_instructions:
        block, incoming_constants = pending.popleft()
        if not in_image(block, load, size):
            continue
        pc = block
        constants = dict(incoming_constants)
        while in_image(pc, load, size) and pc not in visited:
            if len(visited) >= max_instructions:
                break
            visited.add(pc)
            instruction = word(payload, pc - load)
            decoded = decode_word(instruction, pc)
            kind = decoded["kind"]
            opcode = instruction >> 26
            rs = (instruction >> 21) & 0x1F
            rt = (instruction >> 16) & 0x1F
            rd = (instruction >> 11) & 0x1F
            funct = instruction & 0x3F
            imm = instruction & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm

            if opcode == 0x0F:
                constants[rt] = imm << 16
            elif opcode == 0x0D and rs in constants:
                constants[rt] = constants[rs] | imm
            elif opcode == 0x09 and rs in constants:
                constants[rt] = (constants[rs] + simm) & 0xFFFFFFFF
            elif opcode == 0 and funct in (0x21, 0x23) and rs in constants and rt in constants:
                value = constants[rs] + constants[rt]
                if funct == 0x23:
                    value = constants[rs] - constants[rt]
                constants[rd] = value & 0xFFFFFFFF
            elif opcode == 0x03 or opcode in (0x20, 0x21, 0x23, 0x24, 0x25):
                constants.pop(rt, None)
            elif opcode == 0 and funct in (0x08, 0x09):
                constants.pop(rd, None)

            if kind == "call":
                target = decoded["target"]
                if in_image(target, load, size):
                    calls.add(target)
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                continuation = pc + 8
                if in_image(continuation, load, size):
                    pending.append((continuation, constants.copy()))
                break
            if kind == "indirect_call":
                target = constants.get(rs)
                if target is not None and in_image(target, load, size):
                    calls.add(target)
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                continuation = pc + 8
                if in_image(continuation, load, size):
                    pending.append((continuation, constants.copy()))
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
                    pending.append((target, constants.copy()))
                break
            if kind == "jump_register":
                target = constants.get(rs)
                if target is not None and in_image(target, load, size):
                    pending.append((target, constants.copy()))
                break
            if kind == "branch":
                delay = pc + 4
                if in_image(delay, load, size):
                    visited.add(delay)
                for successor in (decoded["target"], pc + 8):
                    if in_image(successor, load, size):
                        pending.append((successor, constants.copy()))
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
