#!/usr/bin/env python3
"""Generate a local native translation-unit prototype from a PS-X EXE.

The output is a block-threaded translation: one C++ function per basic block,
registered by the guest address of the block's first instruction. A block ends
at a call, jump, branch, return or trap, so the dispatcher can follow calls,
loops and returns without re-entering the source function from its entry.

The output is intentionally required to live outside the repository. It is a
derived local analysis artifact and must never be committed or distributed.
Only a safe instruction subset is emitted as C++; unsupported instructions fail
loudly at runtime.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from mips_cfg import discover_reachable_functions, in_image, walk_function
from mips_decode import decode_word

CONTROL_KINDS = {
    "call",
    "jump",
    "branch",
    "return",
    "indirect_call",
    "jump_register",
    "trap",
}


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def cpp_reg(index: int) -> str:
    return f"cpu.gpr[{index}]"


def ensure_outside_repo(output: Path) -> None:
    repo = Path(__file__).resolve().parents[1]
    try:
        output.relative_to(repo)
    except ValueError:
        return
    raise ValueError("generated translation units must be written outside the repository")


def unsupported(address: int, instruction: int) -> list[str]:
    return [
        f"    r6::unsupported_instruction(cpu, 0x{address:08X}u,"
        f" 0x{instruction:08X}u);"
    ]


def translate(instruction: int, address: int, decoded: dict) -> list[str]:
    opcode = instruction >> 26
    rs = (instruction >> 21) & 0x1F
    rt = (instruction >> 16) & 0x1F
    rd = (instruction >> 11) & 0x1F
    shamt = (instruction >> 6) & 0x1F
    funct = instruction & 0x3F
    imm = instruction & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm

    if instruction == 0:
        return ["    cpu.pc += 4;  // nop"]

    if opcode == 0:
        if funct in (0x20, 0x21):
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rs)} + {cpp_reg(rt)};",
                "    cpu.pc += 4;",
            ]
        if funct in (0x22, 0x23):
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rs)} - {cpp_reg(rt)};",
                "    cpu.pc += 4;",
            ]
        if funct == 0x24:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rs)} & {cpp_reg(rt)};",
                "    cpu.pc += 4;",
            ]
        if funct == 0x25:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rs)} | {cpp_reg(rt)};",
                "    cpu.pc += 4;",
            ]
        if funct == 0x26:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rs)} ^ {cpp_reg(rt)};",
                "    cpu.pc += 4;",
            ]
        if funct == 0x27:
            return [
                f"    {cpp_reg(rd)} = ~({cpp_reg(rs)} | {cpp_reg(rt)});",
                "    cpu.pc += 4;",
            ]
        if funct == 0x2A:
            return [
                f"    {cpp_reg(rd)} ="
                f" static_cast<std::int32_t>({cpp_reg(rs)}) <"
                f" static_cast<std::int32_t>({cpp_reg(rt)}) ? 1u : 0u;",
                "    cpu.pc += 4;",
            ]
        if funct == 0x2B:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rs)} < {cpp_reg(rt)} ? 1u : 0u;",
                "    cpu.pc += 4;",
            ]
        if funct == 0x00:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rt)} << {shamt};",
                "    cpu.pc += 4;",
            ]
        if funct == 0x02:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rt)} >> {shamt};",
                "    cpu.pc += 4;",
            ]
        if funct == 0x03:
            return [
                f"    {cpp_reg(rd)} = static_cast<std::uint32_t>("
                f"static_cast<std::int32_t>({cpp_reg(rt)}) >> {shamt});",
                "    cpu.pc += 4;",
            ]
        if funct == 0x04:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rt)} <<"
                f" ({cpp_reg(rs)} & 31u);",
                "    cpu.pc += 4;",
            ]
        if funct == 0x06:
            return [
                f"    {cpp_reg(rd)} = {cpp_reg(rt)} >>"
                f" ({cpp_reg(rs)} & 31u);",
                "    cpu.pc += 4;",
            ]
        if funct == 0x07:
            return [
                f"    {cpp_reg(rd)} = static_cast<std::uint32_t>("
                f"static_cast<std::int32_t>({cpp_reg(rt)}) >>"
                f" ({cpp_reg(rs)} & 31u));",
                "    cpu.pc += 4;",
            ]
        if funct == 0x10:
            return [f"    {cpp_reg(rd)} = cpu.hi;", "    cpu.pc += 4;"]
        if funct == 0x11:
            return [f"    cpu.hi = {cpp_reg(rs)};", "    cpu.pc += 4;"]
        if funct == 0x12:
            return [f"    {cpp_reg(rd)} = cpu.lo;", "    cpu.pc += 4;"]
        if funct == 0x13:
            return [f"    cpu.lo = {cpp_reg(rs)};", "    cpu.pc += 4;"]
        if funct == 0x18:
            return [
                f"    const std::int64_t product_0x{address:08X} ="
                f" static_cast<std::int64_t>(static_cast<std::int32_t>("
                f"{cpp_reg(rs)})) * static_cast<std::int64_t>("
                f"static_cast<std::int32_t>({cpp_reg(rt)}));",
                f"    cpu.lo = static_cast<std::uint32_t>(product_0x{address:08X});",
                "    cpu.hi = static_cast<std::uint32_t>("
                f"static_cast<std::uint64_t>(product_0x{address:08X}) >> 32);",
                "    cpu.pc += 4;",
            ]
        if funct == 0x19:
            return [
                f"    const std::uint64_t product_0x{address:08X} ="
                f" static_cast<std::uint64_t>({cpp_reg(rs)}) *"
                f" static_cast<std::uint64_t>({cpp_reg(rt)});",
                f"    cpu.lo = static_cast<std::uint32_t>(product_0x{address:08X});",
                f"    cpu.hi = static_cast<std::uint32_t>(product_0x{address:08X} >> 32);",
                "    cpu.pc += 4;",
            ]
        if funct == 0x1A:
            return [
                f"    const std::int32_t numerator_0x{address:08X} ="
                f" static_cast<std::int32_t>({cpp_reg(rs)});",
                f"    const std::int32_t denominator_0x{address:08X} ="
                f" static_cast<std::int32_t>({cpp_reg(rt)});",
                f"    if (denominator_0x{address:08X} != 0) {{",
                "        cpu.lo = static_cast<std::uint32_t>("
                f"numerator_0x{address:08X} / denominator_0x{address:08X});",
                "        cpu.hi = static_cast<std::uint32_t>("
                f"numerator_0x{address:08X} % denominator_0x{address:08X});",
                "    }",
                "    cpu.pc += 4;",
            ]
        if funct == 0x1B:
            return [
                f"    if ({cpp_reg(rt)} != 0) {{",
                f"        cpu.lo = {cpp_reg(rs)} / {cpp_reg(rt)};",
                f"        cpu.hi = {cpp_reg(rs)} % {cpp_reg(rt)};",
                "    }",
                "    cpu.pc += 4;",
            ]

    if opcode == 0x08:  # addi
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} + {simm};",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x09:  # addiu
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} + {simm};",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0A:  # slti
        return [
            f"    {cpp_reg(rt)} = static_cast<std::int32_t>({cpp_reg(rs)}) <"
            f" {simm} ? 1u : 0u;",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0B:  # sltiu
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} <"
            f" static_cast<std::uint32_t>({simm}) ? 1u : 0u;",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0C:
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} & 0x{imm:04X}u;",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0D:
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} | 0x{imm:04X}u;",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0E:
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} ^ 0x{imm:04X}u;",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0F:
        return [
            f"    {cpp_reg(rt)} = 0x{imm:04X}u << 16;",
            "    cpu.pc += 4;",
        ]
    if opcode in (0x20, 0x21, 0x23, 0x24, 0x25):
        address_name = f"address_{address:08X}"
        operation = {
            0x20: f"static_cast<std::int32_t>(static_cast<std::int8_t>(memory.read8({address_name})))",
            0x21: f"static_cast<std::int32_t>(static_cast<std::int16_t>(memory.read16({address_name})))",
            0x23: f"memory.read32({address_name})",
            0x24: f"memory.read8({address_name})",
            0x25: f"memory.read16({address_name})",
        }[opcode]
        return [
            f"    const std::uint32_t {address_name} = {cpp_reg(rs)} + {simm};",
            f"    {cpp_reg(rt)} = static_cast<std::uint32_t>({operation});",
            "    cpu.pc += 4;",
        ]
    if opcode in (0x28, 0x29, 0x2B):
        address_name = f"address_{address:08X}"
        operation = {
            0x28: f"memory.write8({address_name}, static_cast<std::uint8_t>({cpp_reg(rt)}));",
            0x29: f"memory.write16({address_name}, static_cast<std::uint16_t>({cpp_reg(rt)}));",
            0x2B: f"memory.write32({address_name}, {cpp_reg(rt)});",
        }[opcode]
        return [
            f"    const std::uint32_t {address_name} = {cpp_reg(rs)} + {simm};",
            f"    {operation}",
            "    cpu.pc += 4;",
        ]

    return unsupported(address, instruction)


def translate_delay_slot(payload: bytes, load_address: int, address: int) -> list[str]:
    offset = address - load_address
    if offset < 0 or offset + 4 > len(payload):
        return [f"    r6::unsupported_instruction(cpu, 0x{address:08X}u, 0u);"]
    instruction = word(payload, offset)
    decoded = decode_word(instruction, address)
    if decoded["kind"] in CONTROL_KINDS:
        return [f"    r6::unsupported_instruction(cpu, 0x{address:08X}u,"
                f" 0x{instruction:08X}u);"]
    return translate(instruction, address, decoded)


def translate_control(
    instruction: int,
    address: int,
    decoded: dict,
    payload: bytes,
    load_address: int,
) -> list[str]:
    delay = translate_delay_slot(payload, load_address, address + 4)
    opcode = instruction >> 26
    rs = (instruction >> 21) & 0x1F
    rt = (instruction >> 16) & 0x1F
    kind = decoded["kind"]

    if kind == "call":
        return [
            f"    cpu.gpr[31] = 0x{address + 8:08X}u;",
            *delay,
            f"    cpu.pc = 0x{decoded['target']:08X}u;",
            "    return cpu.pc;",
        ]
    if kind == "jump":
        return [
            *delay,
            f"    cpu.pc = 0x{decoded['target']:08X}u;",
            "    return cpu.pc;",
        ]
    if kind == "return":
        return [*delay, f"    cpu.pc = cpu.gpr[{rs}];", "    return cpu.pc;"]
    if kind == "indirect_call":
        rd = (instruction >> 11) & 0x1F
        return [
            f"    cpu.gpr[{rd}] = 0x{address + 8:08X}u;",
            *delay,
            f"    cpu.pc = cpu.gpr[{rs}];",
            "    return cpu.pc;",
        ]
    if kind == "jump_register":
        return [
            *delay,
            f"    cpu.pc = cpu.gpr[{rs}];",
            "    return cpu.pc;",
        ]
    if kind == "branch":
        target = decoded["target"]
        if opcode == 0x01:
            condition = (
                f"static_cast<std::int32_t>(cpu.gpr[{rs}]) < 0"
                if rt == 0
                else f"static_cast<std::int32_t>(cpu.gpr[{rs}]) >= 0"
            )
        elif opcode in (0x04, 0x14):
            condition = f"cpu.gpr[{rs}] == cpu.gpr[{rt}]"
        elif opcode in (0x05, 0x15):
            condition = f"cpu.gpr[{rs}] != cpu.gpr[{rt}]"
        elif opcode in (0x06, 0x16):
            condition = f"static_cast<std::int32_t>(cpu.gpr[{rs}]) <= 0"
        else:
            condition = f"static_cast<std::int32_t>(cpu.gpr[{rs}]) > 0"
        return [
            f"    const bool taken_0x{address:08X} = {condition};",
            *delay,
            f"    cpu.pc = taken_0x{address:08X} ? 0x{target:08X}u :"
            f" 0x{address + 8:08X}u;",
            "    return cpu.pc;",
        ]
    return unsupported(address, instruction)


def emit_block(
    payload: bytes,
    load_address: int,
    entry: int,
    leaders: set[int],
    max_instructions: int,
) -> tuple[list[str], int, bool]:
    """Emit one block. Returns (lines, next_address, ended_at_control)."""
    lines = [
        f"std::uint32_t block_{entry:08X}(r6::CpuState& cpu, r6::Memory& memory,",
        "                            r6::PlatformServices& services) {",
        "    (void)memory;",
        "    (void)services;",
        f"    // Block at 0x{entry:08X}",
    ]
    pc = entry
    count = 0
    while count < max_instructions and in_image(pc, load_address, len(payload)):
        if pc != entry and pc in leaders:
            break
        instruction = word(payload, pc - load_address)
        decoded = decode_word(instruction, pc)
        lines.append(f"    // 0x{pc:08X}: {decoded['text']}")
        if decoded["kind"] in CONTROL_KINDS:
            control = translate_control(
                instruction, pc, decoded, payload, load_address
            )
            lines.extend(control)
            lines.extend(["}", ""])
            return lines, pc, True
        translated = translate(instruction, pc, decoded)
        lines.extend(translated)
        count += 1
        if any("unsupported_instruction" in line for line in translated):
            lines.extend(["}", ""])
            return lines, pc, False
        pc += 4

    lines.extend([f"    cpu.pc = 0x{pc:08X}u;", "    return cpu.pc;", "}", ""])
    return lines, pc, False


def collect_leaders(
    payload: bytes,
    load_address: int,
    initial_pc: int,
    extra_entries: list[int],
    max_functions: int,
    max_instructions: int,
) -> tuple[list[int], set[int]]:
    functions = discover_reachable_functions(
        payload,
        load_address,
        initial_pc,
        max_functions=max_functions,
        max_instructions_per_function=max_instructions,
    )
    for entry in extra_entries:
        if entry not in functions and in_image(entry, load_address, len(payload)):
            functions.append(entry)

    leaders: set[int] = set()
    delay_slots: set[int] = set()
    for entry in functions:
        visited, _ = walk_function(
            payload,
            load_address,
            len(payload),
            entry,
            max_instructions,
        )
        leaders.add(entry)
        for pc in visited:
            instruction = word(payload, pc - load_address)
            decoded = decode_word(instruction, pc)
            kind = decoded["kind"]
            if kind not in CONTROL_KINDS:
                continue
            if decoded.get("delay_slot"):
                delay_slots.add(pc + 4)
            if kind in ("call", "indirect_call"):
                if pc + 8 in visited:
                    leaders.add(pc + 8)
            elif kind == "branch":
                if decoded["target"] in visited:
                    leaders.add(decoded["target"])
                if pc + 8 in visited:
                    leaders.add(pc + 8)
            elif kind == "jump":
                if decoded["target"] in visited:
                    leaders.add(decoded["target"])

    leaders.difference_update(delay_slots)
    leaders = {
        leader
        for leader in leaders
        if in_image(leader, load_address, len(payload))
    }
    return functions, leaders


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe_path", type=Path, help="local PS-X EXE analysis copy")
    parser.add_argument("output_path", type=Path, help="local output path outside the repo")
    parser.add_argument("--max-instructions", type=int, default=20000)
    parser.add_argument("--max-functions", type=int, default=64)
    parser.add_argument("--max-block-instructions", type=int, default=256)
    parser.add_argument("--max-blocks", type=int, default=4096)
    parser.add_argument(
        "--extra-entry",
        action="append",
        type=lambda value: int(value, 0),
        default=[],
        help="add a block entry for an indirectly called function",
    )
    args = parser.parse_args()

    exe = args.exe_path.resolve()
    output = args.output_path.resolve()
    ensure_outside_repo(output)

    data = exe.read_bytes()
    if data[:8] != b"PS-X EXE":
        parser.error("input does not have a PS-X EXE header")
    load_address = word(data, 0x18)
    payload_size = word(data, 0x1C)
    initial_pc = word(data, 0x10)
    payload = data[0x800 : 0x800 + payload_size]

    functions, leaders = collect_leaders(
        payload,
        load_address,
        initial_pc,
        args.extra_entry,
        args.max_functions,
        args.max_instructions,
    )
    pending = sorted(leaders)
    emitted: set[int] = set()
    blocks: list[tuple[int, list[str]]] = []
    while pending and len(blocks) < args.max_blocks:
        leader = pending.pop(0)
        if leader in emitted:
            continue
        emitted.add(leader)
        lines, next_address, ended_at_control = emit_block(
            payload,
            load_address,
            leader,
            leaders,
            args.max_block_instructions,
        )
        blocks.append((leader, lines))
        if (
            not ended_at_control
            and next_address not in leaders
            and in_image(next_address, load_address, len(payload))
        ):
            leaders.add(next_address)
            pending.append(next_address)

    lines = [
        "// GENERATED LOCALLY FROM A USER-SUPPLIED PS-X EXE.",
        "// Do not commit or distribute this file.",
        '#include "r6/function_dispatch.h"',
        '#include "r6/native_runtime.h"',
        "",
        "#include <cstddef>",
        "",
        "namespace r6_generated {",
        "",
    ]
    for _, block in blocks:
        lines.extend(block)
    lines.append("std::size_t block_count() {")
    lines.append(f"    return {len(blocks)}u;")
    lines.append("}")
    lines.append("")
    lines.append("void register_functions(r6::FunctionDispatcher& dispatcher) {")
    for entry, _ in blocks:
        lines.append(
            f"    dispatcher.register_function(0x{entry:08X}u, &block_{entry:08X});"
        )
    lines.extend(["}", "", "}  // namespace r6_generated", ""])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"Functions discovered: {len(functions)}")
    print(f"Blocks emitted: {len(blocks)}")
    if pending:
        print(f"Blocks left for a later slice: {len(pending)}")
    print(f"Wrote local native translation prototype: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
