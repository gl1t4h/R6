"""Small, dependency-free MIPS R3000A instruction decoder.

The decoder returns structured metadata and formatted text. It does not read
disc images or emit game code.
"""

from __future__ import annotations


REGISTERS = (
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
    "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
    "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
)


def s16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def reg(index: int) -> str:
    return REGISTERS[index & 0x1F]


def decode_word(instruction: int, pc: int) -> dict:
    """Decode one little-endian MIPS instruction word."""
    instruction &= 0xFFFFFFFF
    opcode = instruction >> 26
    rs = (instruction >> 21) & 0x1F
    rt = (instruction >> 16) & 0x1F
    rd = (instruction >> 11) & 0x1F
    shamt = (instruction >> 6) & 0x1F
    funct = instruction & 0x3F
    imm = instruction & 0xFFFF
    simm = s16(imm)

    def result(kind: str, text: str, **extra: object) -> dict:
        value = {
            "word": f"0x{instruction:08X}",
            "pc": f"0x{pc:08X}",
            "kind": kind,
            "mnemonic": text.split(" ", 1)[0],
            "text": text,
            "control": False,
        }
        value.update(extra)
        return value

    if opcode == 0:
        if instruction == 0:
            return result("normal", "nop")
        specials = {
            0x20: ("add", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x21: ("addu", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x22: ("sub", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x23: ("subu", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x24: ("and", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x25: ("or", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x26: ("xor", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x27: ("nor", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x2A: ("slt", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x2B: ("sltu", f"{reg(rd)}, {reg(rs)}, {reg(rt)}"),
            0x00: ("sll", f"{reg(rd)}, {reg(rt)}, {shamt}"),
            0x02: ("srl", f"{reg(rd)}, {reg(rt)}, {shamt}"),
            0x03: ("sra", f"{reg(rd)}, {reg(rt)}, {shamt}"),
            0x04: ("sllv", f"{reg(rd)}, {reg(rt)}, {reg(rs)}"),
            0x06: ("srlv", f"{reg(rd)}, {reg(rt)}, {reg(rs)}"),
            0x07: ("srav", f"{reg(rd)}, {reg(rt)}, {reg(rs)}"),
            0x10: ("mfhi", reg(rd)),
            0x12: ("mflo", reg(rd)),
            0x11: ("mthi", reg(rs)),
            0x13: ("mtlo", reg(rs)),
            0x18: ("mult", f"{reg(rs)}, {reg(rt)}"),
            0x19: ("multu", f"{reg(rs)}, {reg(rt)}"),
            0x1A: ("div", f"{reg(rs)}, {reg(rt)}"),
            0x1B: ("divu", f"{reg(rs)}, {reg(rt)}"),
        }
        if funct in specials:
            mnemonic, operands = specials[funct]
            return result("normal", f"{mnemonic} {operands}")
        if funct == 0x08:
            return result(
                "return" if rs == 31 else "jump_register",
                f"jr {reg(rs)}",
                control=True,
                delay_slot=True,
            )
        if funct == 0x09:
            return result(
                "indirect_call",
                f"jalr {reg(rd)}, {reg(rs)}",
                control=True,
                delay_slot=True,
            )
        if funct == 0x0C:
            return result("trap", "syscall", control=True)
        if funct == 0x0D:
            return result("trap", "break", control=True)
        return result("unknown", f".word 0x{instruction:08X}")

    if opcode in (0x02, 0x03):
        target = ((pc + 4) & 0xF0000000) | ((instruction & 0x03FFFFFF) << 2)
        mnemonic = "jal" if opcode == 0x03 else "j"
        return result(
            "call" if opcode == 0x03 else "jump",
            f"{mnemonic} 0x{target:08X}",
            target=target,
            control=True,
            delay_slot=True,
        )

    branches = {
        0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
        0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl",
    }
    if opcode == 0x01:
        regimm = {
            0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl",
            0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall",
            0x13: "bgezall",
        }
        if rt in regimm:
            target = pc + 4 + (simm << 2)
            return result(
                "branch",
                f"{regimm[rt]} {reg(rs)}, 0x{target:08X}",
                target=target,
                control=True,
                delay_slot=True,
            )

    if opcode in branches:
        target = pc + 4 + (simm << 2)
        if opcode in (0x06, 0x07, 0x16, 0x17):
            operands = f"{reg(rs)}, 0x{target:08X}"
        else:
            operands = f"{reg(rs)}, {reg(rt)}, 0x{target:08X}"
        return result(
            "branch",
            f"{branches[opcode]} {operands}",
            target=target,
            control=True,
            delay_slot=True,
        )

    immediate = {
        0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu",
        0x0C: "andi", 0x0D: "ori", 0x0E: "xori",
    }
    if opcode in immediate:
        value = simm if opcode in (0x08, 0x09, 0x0A, 0x0B) else f"0x{imm:04X}"
        return result("normal", f"{immediate[opcode]} {reg(rt)}, {reg(rs)}, {value}")
    if opcode == 0x0F:
        return result("normal", f"lui {reg(rt)}, 0x{imm:04X}")

    memory = {
        0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu",
        0x28: "sb", 0x29: "sh", 0x2B: "sw",
    }
    if opcode in memory:
        return result("memory", f"{memory[opcode]} {reg(rt)}, {simm}({reg(rs)})")

    if opcode in (0x10, 0x11, 0x12):
        cop = {0x10: "cop0", 0x11: "cop1", 0x12: "cop2"}[opcode]
        return result("coprocessor", f"{cop} 0x{instruction & 0x03FFFFFF:07X}")

    return result("unknown", f".word 0x{instruction:08X}")
