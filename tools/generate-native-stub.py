#!/usr/bin/env python3
"""Generate a local native translation-unit prototype from a PS-X EXE.

The output is intentionally required to live outside the repository. It is a
derived local analysis artifact and must never be committed or distributed.
Only a small safe instruction subset is emitted as C++; unsupported
instructions fail loudly at runtime.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from mips_decode import decode_word


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


def translate(instruction: int, address: int, decoded: dict) -> list[str]:
    opcode = instruction >> 26
    rs = (instruction >> 21) & 0x1F
    rt = (instruction >> 16) & 0x1F
    rd = (instruction >> 11) & 0x1F
    shamt = (instruction >> 6) & 0x1F
    imm = instruction & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    kind = decoded["kind"]

    if instruction == 0:
        return ["    cpu.pc += 4;  // nop"]
    if opcode == 0 and (instruction & 0x3F) == 0x21:
        return [
            f"    {cpp_reg(rd)} = {cpp_reg(rs)} + {cpp_reg(rt)};",
            "    cpu.pc += 4;",
        ]
    if opcode == 0 and (instruction & 0x3F) == 0x23:
        return [
            f"    {cpp_reg(rd)} = {cpp_reg(rs)} - {cpp_reg(rt)};",
            "    cpu.pc += 4;",
        ]
    if opcode == 0 and (instruction & 0x3F) == 0x00:
        return [
            f"    {cpp_reg(rd)} = {cpp_reg(rt)} << {shamt};",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x09:
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} + {simm};",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0F:
        return [
            f"    {cpp_reg(rt)} = 0x{imm:04X}u << 16;",
            "    cpu.pc += 4;",
        ]
    if opcode == 0x0D:
        return [
            f"    {cpp_reg(rt)} = {cpp_reg(rs)} | 0x{imm:04X}u;",
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
    if kind == "return":
        return ["    return;  // jr $ra"]

    return [
        f"    r6::unsupported_instruction(cpu, 0x{address:08X}u, 0x{instruction:08X}u);"
    ]


def translate_delay_slot(payload: bytes, load_address: int, address: int) -> list[str]:
    offset = address - load_address
    if offset < 0 or offset + 4 > len(payload):
        return ["    r6::unsupported_instruction(cpu, cpu.pc, 0u);"]
    instruction = word(payload, offset)
    decoded = decode_word(instruction, address)
    if decoded["kind"] in ("call", "jump", "branch", "return", "indirect_call", "trap"):
        return [
            f"    r6::unsupported_instruction(cpu, 0x{address:08X}u, 0x{instruction:08X}u);"
        ]
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
    funct = instruction & 0x3F

    if decoded["kind"] == "call":
        return [
            f"    cpu.gpr[31] = 0x{address + 8:08X}u;",
            *delay,
            f"    cpu.pc = 0x{decoded['target']:08X}u;",
            "    return cpu.pc;",
        ]
    if decoded["kind"] == "jump":
        return [
            *delay,
            f"    cpu.pc = 0x{decoded['target']:08X}u;",
            "    return cpu.pc;",
        ]
    if decoded["kind"] == "return":
        return [*delay, f"    cpu.pc = cpu.gpr[{rs}];", "    return cpu.pc;"]
    if decoded["kind"] == "indirect_call":
        rd = (instruction >> 11) & 0x1F
        return [
            f"    cpu.gpr[{rd}] = 0x{address + 8:08X}u;",
            *delay,
            f"    cpu.pc = cpu.gpr[{rs}];",
            "    return cpu.pc;",
        ]
    if decoded["kind"] == "branch":
        target = decoded["target"]
        if opcode == 0x04:
            condition = f"cpu.gpr[{rs}] == cpu.gpr[{rt}]"
        elif opcode == 0x05:
            condition = f"cpu.gpr[{rs}] != cpu.gpr[{rt}]"
        elif opcode in (0x06, 0x16):
            condition = f"static_cast<std::int32_t>(cpu.gpr[{rs}]) <= 0"
        else:
            condition = f"static_cast<std::int32_t>(cpu.gpr[{rs}]) > 0"
        return [
            f"    const bool taken = {condition};",
            *delay,
            f"    cpu.pc = taken ? 0x{target:08X}u : 0x{address + 8:08X}u;",
            "    return cpu.pc;",
        ]
    return [
        f"    r6::unsupported_instruction(cpu, 0x{address:08X}u, 0x{instruction:08X}u);"
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe_path", type=Path, help="local PS-X EXE analysis copy")
    parser.add_argument("output_path", type=Path, help="local output path outside the repo")
    parser.add_argument("--max-instructions", type=int, default=256)
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
    payload = data[2048 : 2048 + payload_size]

    lines = [
        "// GENERATED LOCALLY FROM A USER-SUPPLIED PS-X EXE.",
        "// Do not commit or distribute this file.",
        '#include "r6/function_dispatch.h"',
        "",
        "namespace r6_generated {",
        "",
        f"std::uint32_t entry(r6::CpuState& cpu, r6::Memory& memory,",
        "                    r6::PlatformServices& services) {",
        "    (void)services;  // platform calls are the next runtime layer",
        f"    // Entry point: 0x{initial_pc:08X}",
    ]

    entry_offset = initial_pc - load_address
    if entry_offset < 0 or entry_offset >= len(payload) or entry_offset % 4:
        parser.error("PS-X EXE entry point is outside the declared payload")

    for offset in range(
        entry_offset,
        min(len(payload), entry_offset + args.max_instructions * 4),
        4,
    ):
        address = load_address + offset
        instruction = word(payload, offset)
        decoded = decode_word(instruction, address)
        lines.append(f"    // 0x{address:08X}: {decoded['text']}")
        if decoded["kind"] in ("call", "jump", "branch", "trap", "indirect_call", "return"):
            lines.extend(translate_control(
                instruction, address, decoded, payload, load_address
            ))
            break
        lines.extend(translate(instruction, address, decoded))

    if lines[-1].strip() != "return cpu.pc;":
        lines.append("    return cpu.pc;")
    lines.extend([
        "}",
        "",
        "void register_functions(r6::FunctionDispatcher& dispatcher) {",
        f"    dispatcher.register_function(0x{initial_pc:08X}u, &entry);",
        "}",
        "",
        "}  // namespace r6_generated",
        "",
    ])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote local native translation prototype: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
