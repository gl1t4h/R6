#include "r6/mips_interpreter.h"

#include <cstddef>
#include <cstdint>

namespace r6 {

namespace {

constexpr std::uint32_t kRamMask = 0x001FFFFFu;      // 2 MiB main RAM
constexpr std::uint32_t kScratchpadBase = 0x1F800000u;
constexpr std::uint32_t kScratchpadMask = 0x3FFu;
constexpr std::uint32_t kHardwareBase = 0x1F801000u;
constexpr std::uint32_t kHardwareEnd = 0x1F803000u;
constexpr std::uint32_t kDmaBase = 0x1F801080u;
constexpr std::uint32_t kDmaEnd = 0x1F801100u;

constexpr std::uint32_t reg(std::uint32_t instruction, std::uint32_t shift) {
    return (instruction >> shift) & 0x1Fu;
}

constexpr std::uint32_t immediate(std::uint32_t instruction) {
    return instruction & 0xFFFFu;
}

constexpr std::uint32_t sign_extend16(std::uint32_t value) {
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFFu)));
}

}  // namespace

MipsInterpreter::MipsInterpreter(Memory& ram, PlatformServices& services)
    : ram_(ram),
      services_(services),
      hardware_(services),
      scratchpad_(0x400, 0),
      cop0_(32, 0),
      gte_data_(32, 0) {}

std::uint32_t MipsInterpreter::read8(std::uint32_t address) {
    const auto physical = address & 0x1FFFFFFFu;
    if (physical <= kRamMask) return ram_.read8(physical & kRamMask);
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        return scratchpad_[physical & kScratchpadMask];
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        return read32(physical & ~3u) >> ((physical & 3u) * 8u);
    }
    ++unknown_hardware_reads_;
    return 0;
}

std::uint32_t MipsInterpreter::read16(std::uint32_t address) {
    const auto physical = address & 0x1FFFFFFFu;
    if (physical <= kRamMask) return ram_.read16(physical & kRamMask);
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~1u;
        return static_cast<std::uint32_t>(scratchpad_[index]) |
               (static_cast<std::uint32_t>(scratchpad_[index + 1]) << 8);
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        return read32(physical & ~3u) >> ((physical & 2u) * 8u);
    }
    ++unknown_hardware_reads_;
    return 0;
}

std::uint32_t MipsInterpreter::read32(std::uint32_t address) {
    const auto physical = address & 0x1FFFFFFFu;
    if (physical <= kRamMask) return ram_.read32(physical & kRamMask);
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~3u;
        return static_cast<std::uint32_t>(scratchpad_[index]) |
               (static_cast<std::uint32_t>(scratchpad_[index + 1]) << 8) |
               (static_cast<std::uint32_t>(scratchpad_[index + 2]) << 16) |
               (static_cast<std::uint32_t>(scratchpad_[index + 3]) << 24);
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        ++hardware_reads_[physical & ~3u];
        return hardware_.read32(physical);
    }
    ++unknown_hardware_reads_;
    return 0;
}

void MipsInterpreter::write8(std::uint32_t address, std::uint32_t value) {
    const auto physical = address & 0x1FFFFFFFu;
    if (physical <= kRamMask) {
        ram_.write8(physical & kRamMask, static_cast<std::uint8_t>(value));
        return;
    }
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        scratchpad_[physical & kScratchpadMask] =
            static_cast<std::uint8_t>(value);
        return;
    }
    ++unknown_hardware_writes_;
}

void MipsInterpreter::write16(std::uint32_t address, std::uint32_t value) {
    const auto physical = address & 0x1FFFFFFFu;
    if (physical <= kRamMask) {
        ram_.write16(physical & kRamMask, static_cast<std::uint16_t>(value));
        return;
    }
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~1u;
        scratchpad_[index] = static_cast<std::uint8_t>(value);
        scratchpad_[index + 1] = static_cast<std::uint8_t>(value >> 8);
        return;
    }
    ++unknown_hardware_writes_;
}

void MipsInterpreter::write32(std::uint32_t address, std::uint32_t value) {
    const auto physical = address & 0x1FFFFFFFu;
    if (physical <= kRamMask) {
        ram_.write32(physical & kRamMask, value);
        return;
    }
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~3u;
        scratchpad_[index] = static_cast<std::uint8_t>(value);
        scratchpad_[index + 1] = static_cast<std::uint8_t>(value >> 8);
        scratchpad_[index + 2] = static_cast<std::uint8_t>(value >> 16);
        scratchpad_[index + 3] = static_cast<std::uint8_t>(value >> 24);
        return;
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        ++hardware_writes_[physical & ~3u];
        if (physical >= kDmaBase && physical < kDmaEnd) {
            // Transfers complete immediately in the hardware map, so a guest
            // polling the channel control register sees the busy bit clear.
            ++dma_writes_;
        }
        hardware_.write32(physical, value);
        return;
    }
    ++unknown_hardware_writes_;
}

bool MipsInterpreter::service_bios_call() {
    return services_.bios_call(cpu_.pc, cpu_, ram_);
}

RunResult MipsInterpreter::run(std::uint64_t max_steps) {
    RunResult result;
    bool in_delay_slot = false;
    std::uint32_t delay_target = 0;

    while (result.steps < max_steps) {
        const auto current_pc = cpu_.pc;
        const bool was_delay_slot = in_delay_slot;
        const auto target = delay_target;
        in_delay_slot = false;

        if (current_pc == 0) {
            result.reason = StopReason::returned;
            result.pc = current_pc;
            return result;
        }

        if (service_bios_call()) {
            ++result.steps;
            cpu_.pc = cpu_.gpr[31];
            if (recent_pcs_.size() >= 16) recent_pcs_.erase(recent_pcs_.begin());
            recent_pcs_.push_back(current_pc);
            continue;
        }

        const auto instruction = read32(current_pc);
        const auto step = execute(instruction, current_pc);
        ++result.steps;

        if (recent_pcs_.size() >= 16) recent_pcs_.erase(recent_pcs_.begin());
        recent_pcs_.push_back(current_pc);

        if (step.unsupported) {
            result.reason = StopReason::unsupported_instruction;
            result.pc = current_pc;
            result.instruction = instruction;
            return result;
        }

        if (step.halted) {
            result.reason = StopReason::guest_break;
            result.pc = current_pc;
            result.instruction = instruction;
            return result;
        }

        cpu_.gpr[0] = 0;
        if (step.branch) {
            in_delay_slot = true;
            delay_target = step.target;
        }

        if (was_delay_slot) {
            cpu_.pc = target;
        } else if (step.branch) {
            cpu_.pc = current_pc + 4;
        } else {
            cpu_.pc = current_pc + 4;
        }
    }

    result.reason = StopReason::step_budget;
    result.pc = cpu_.pc;
    return result;
}

MipsInterpreter::StepResult MipsInterpreter::execute(
    std::uint32_t instruction,
    std::uint32_t pc) {
    StepResult result;
    auto& gpr = cpu_.gpr;
    const auto opcode = instruction >> 26;
    const auto rs = reg(instruction, 21);
    const auto rt = reg(instruction, 16);
    const auto rd = reg(instruction, 11);
    const auto shamt = reg(instruction, 6);

    switch (opcode) {
        case 0x00: {  // SPECIAL
            switch (instruction & 0x3Fu) {
                case 0x00:
                    gpr[rd] = gpr[rt] << shamt;
                    break;
                case 0x02:
                    gpr[rd] = gpr[rt] >> shamt;
                    break;
                case 0x03:
                    gpr[rd] = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(gpr[rt]) >> shamt);
                    break;
                case 0x04:
                    gpr[rd] = gpr[rt] << (gpr[rs] & 0x1Fu);
                    break;
                case 0x06:
                    gpr[rd] = gpr[rt] >> (gpr[rs] & 0x1Fu);
                    break;
                case 0x07:
                    gpr[rd] = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(gpr[rt]) >>
                        static_cast<int>(gpr[rs] & 0x1Fu));
                    break;
                case 0x08:
                    result.branch = true;
                    result.target = gpr[rs];
                    break;
                case 0x09: {
                    const auto link = pc + 8;
                    result.branch = true;
                    result.target = gpr[rs];
                    gpr[rd] = link;
                    break;
                }
                case 0x0C:  // syscall
                    break;
                case 0x0D:  // break
                    result.halted = true;
                    break;
                case 0x10:
                    gpr[rd] = cpu_.hi;
                    break;
                case 0x11:
                    cpu_.hi = gpr[rs];
                    break;
                case 0x12:
                    gpr[rd] = cpu_.lo;
                    break;
                case 0x13:
                    cpu_.lo = gpr[rs];
                    break;
                case 0x18: {
                    const auto product =
                        static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[rs])) *
                        static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[rt]));
                    cpu_.lo = static_cast<std::uint32_t>(product);
                    cpu_.hi = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(product) >> 32);
                    break;
                }
                case 0x19: {
                    const auto product =
                        static_cast<std::uint64_t>(gpr[rs]) *
                        static_cast<std::uint64_t>(gpr[rt]);
                    cpu_.lo = static_cast<std::uint32_t>(product);
                    cpu_.hi = static_cast<std::uint32_t>(product >> 32);
                    break;
                }
                case 0x1A: {
                    const auto numerator = static_cast<std::int32_t>(gpr[rs]);
                    const auto denominator = static_cast<std::int32_t>(gpr[rt]);
                    if (denominator != 0) {
                        cpu_.lo = static_cast<std::uint32_t>(
                            numerator / denominator);
                        cpu_.hi = static_cast<std::uint32_t>(
                            numerator % denominator);
                    }
                    break;
                }
                case 0x1B: {
                    if (gpr[rt] != 0) {
                        cpu_.lo = gpr[rs] / gpr[rt];
                        cpu_.hi = gpr[rs] % gpr[rt];
                    }
                    break;
                }
                case 0x20:
                case 0x21:
                    gpr[rd] = gpr[rs] + gpr[rt];
                    break;
                case 0x22:
                case 0x23:
                    gpr[rd] = gpr[rs] - gpr[rt];
                    break;
                case 0x24:
                    gpr[rd] = gpr[rs] & gpr[rt];
                    break;
                case 0x25:
                    gpr[rd] = gpr[rs] | gpr[rt];
                    break;
                case 0x26:
                    gpr[rd] = gpr[rs] ^ gpr[rt];
                    break;
                case 0x27:
                    gpr[rd] = ~(gpr[rs] | gpr[rt]);
                    break;
                case 0x2A:
                    gpr[rd] = static_cast<std::int32_t>(gpr[rs]) <
                                      static_cast<std::int32_t>(gpr[rt])
                                  ? 1u
                                  : 0u;
                    break;
                case 0x2B:
                    gpr[rd] = gpr[rs] < gpr[rt] ? 1u : 0u;
                    break;
                default:
                    result.unsupported = true;
                    break;
            }
            break;
        }
        case 0x01: {  // REGIMM
            const auto offset = sign_extend16(immediate(instruction)) << 2;
            const auto target = pc + 4 + offset;
            switch (rt) {
                case 0x00:  // bltz
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) < 0)) {
                        result.target = pc + 8;
                    }
                    break;
                case 0x01:  // bgez
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) >= 0)) {
                        result.target = pc + 8;
                    }
                    break;
                case 0x10:  // bltzal
                    gpr[31] = pc + 8;
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) < 0)) {
                        result.target = pc + 8;
                    }
                    break;
                case 0x11:  // bgezal
                    gpr[31] = pc + 8;
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) >= 0)) {
                        result.target = pc + 8;
                    }
                    break;
                default:
                    result.unsupported = true;
                    break;
            }
            break;
        }
        case 0x02:  // j
            result.branch = true;
            result.target = (pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
            break;
        case 0x03:  // jal
            gpr[31] = pc + 8;
            result.branch = true;
            result.target = (pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
            break;
        case 0x04:  // beq
            result.branch = true;
            result.target = gpr[rs] == gpr[rt]
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x05:  // bne
            result.branch = true;
            result.target = gpr[rs] != gpr[rt]
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x06:  // blez
            result.branch = true;
            result.target = static_cast<std::int32_t>(gpr[rs]) <= 0
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x07:  // bgtz
            result.branch = true;
            result.target = static_cast<std::int32_t>(gpr[rs]) > 0
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x08:  // addi
        case 0x09:  // addiu
            gpr[rt] = gpr[rs] + sign_extend16(immediate(instruction));
            break;
        case 0x0A:  // slti
            gpr[rt] = static_cast<std::int32_t>(gpr[rs]) <
                              static_cast<std::int32_t>(sign_extend16(immediate(instruction)))
                          ? 1u
                          : 0u;
            break;
        case 0x0B:  // sltiu
            gpr[rt] = gpr[rs] < sign_extend16(immediate(instruction)) ? 1u : 0u;
            break;
        case 0x0C:  // andi
            gpr[rt] = gpr[rs] & immediate(instruction);
            break;
        case 0x0D:  // ori
            gpr[rt] = gpr[rs] | immediate(instruction);
            break;
        case 0x0E:  // xori
            gpr[rt] = gpr[rs] ^ immediate(instruction);
            break;
        case 0x0F:  // lui
            gpr[rt] = immediate(instruction) << 16;
            break;
        case 0x10:  // COP0
            if (rs == 0x00) {
                gpr[rt] = cop0_[rd];
            } else if (rs == 0x04) {
                cop0_[rd] = gpr[rt];
            } else if ((instruction & 0x3Fu) == 0x10) {  // rfe
                const auto status = cop0_[12];
                cop0_[12] = (status & ~0x3Fu) | ((status >> 2) & 0xFu);
            } else {
                result.unsupported = true;
            }
            break;
        case 0x12: {  // COP2 / GTE
            if (rs == 0x00) {
                gpr[rt] = gte_data_[rd];
            } else if (rs == 0x02) {
                gpr[rt] = gte_data_[rd];
            } else if (rs == 0x04) {
                gte_data_[rd] = gpr[rt];
            } else if (rs == 0x06) {
                gte_data_[rd] = gpr[rt];
            } else {
                // Geometry maths is not implemented yet; the command is
                // accepted so execution continues.
                ++gte_commands_;
            }
            break;
        }
        case 0x20:  // lb
            gpr[rt] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(
                    static_cast<std::int8_t>(read8(gpr[rs] + sign_extend16(immediate(instruction))))));
            break;
        case 0x21:  // lh
            gpr[rt] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(
                    static_cast<std::int16_t>(read16(gpr[rs] + sign_extend16(immediate(instruction))))));
            break;
        case 0x22: {  // lwl
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            const auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    gpr[rt] = (gpr[rt] & 0x00FFFFFFu) | (aligned << 24);
                    break;
                case 1:
                    gpr[rt] = (gpr[rt] & 0x0000FFFFu) | (aligned << 16);
                    break;
                case 2:
                    gpr[rt] = (gpr[rt] & 0x000000FFu) | (aligned << 8);
                    break;
                default:
                    gpr[rt] = aligned;
                    break;
            }
            break;
        }
        case 0x23:  // lw
            gpr[rt] = read32(gpr[rs] + sign_extend16(immediate(instruction)));
            break;
        case 0x24:  // lbu
            gpr[rt] = read8(gpr[rs] + sign_extend16(immediate(instruction))) & 0xFFu;
            break;
        case 0x25:  // lhu
            gpr[rt] = read16(gpr[rs] + sign_extend16(immediate(instruction))) & 0xFFFFu;
            break;
        case 0x26: {  // lwr
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            const auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    gpr[rt] = aligned;
                    break;
                case 1:
                    gpr[rt] = (gpr[rt] & 0xFF000000u) | (aligned >> 8);
                    break;
                case 2:
                    gpr[rt] = (gpr[rt] & 0xFFFF0000u) | (aligned >> 16);
                    break;
                default:
                    gpr[rt] = (gpr[rt] & 0xFFFFFF00u) | (aligned >> 24);
                    break;
            }
            break;
        }
        case 0x28:  // sb
            write8(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt] & 0xFFu);
            break;
        case 0x29:  // sh
            write16(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt] & 0xFFFFu);
            break;
        case 0x2A: {  // swl
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    aligned = (aligned & 0xFFFFFF00u) | (gpr[rt] >> 24);
                    break;
                case 1:
                    aligned = (aligned & 0xFFFF0000u) | (gpr[rt] >> 16);
                    break;
                case 2:
                    aligned = (aligned & 0xFF000000u) | (gpr[rt] >> 8);
                    break;
                default:
                    aligned = gpr[rt];
                    break;
            }
            write32(address & ~3u, aligned);
            break;
        }
        case 0x2B:  // sw
            write32(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt]);
            break;
        case 0x2E: {  // swr
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    aligned = gpr[rt];
                    break;
                case 1:
                    aligned = (aligned & 0x000000FFu) | (gpr[rt] << 8);
                    break;
                case 2:
                    aligned = (aligned & 0x0000FFFFu) | (gpr[rt] << 16);
                    break;
                default:
                    aligned = (aligned & 0x00FFFFFFu) | (gpr[rt] << 24);
                    break;
            }
            write32(address & ~3u, aligned);
            break;
        }
        case 0x32:  // lwc2
            gte_data_[rt] = read32(gpr[rs] + sign_extend16(immediate(instruction)));
            break;
        case 0x3A:  // swc2
            write32(gpr[rs] + sign_extend16(immediate(instruction)), gte_data_[rt]);
            break;
        default:
            result.unsupported = true;
            break;
    }

    return result;
}

}  // namespace r6
