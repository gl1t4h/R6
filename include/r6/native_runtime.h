#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "r6/cpu_state.h"
#include "r6/gte.h"
#include "r6/hardware_map.h"
#include "r6/memory.h"

namespace r6 {

// Memory path for translated blocks. It mirrors the interpreter's bus so a
// block can touch RAM, scratchpad, CD-ROM and the hardware registers through
// the same model. The map is created for one block, matching the per-block
// oracle checkpoint used by the differential harness.
class NativeBus {
public:
    NativeBus(Memory& ram, PlatformServices& services)
        : ram_(ram), hardware_(services) {}

    std::uint32_t read8(std::uint32_t address) {
        const auto physical = address & 0x1FFFFFFFu;
        if (hardware_.is_cdrom(physical)) {
            return hardware_.read_cdrom(physical);
        }
        if (physical <= kRamMask) return ram_.read8(physical);
        if (physical >= kScratchpadBase &&
            physical <= kScratchpadBase + kScratchpadMask) {
            return scratchpad_[physical & kScratchpadMask];
        }
        if (physical >= kHardwareBase && physical < kHardwareEnd) {
            return read32(physical & ~3u) >> ((physical & 3u) * 8u);
        }
        return 0;
    }

    std::uint32_t read16(std::uint32_t address) {
        const auto physical = address & 0x1FFFFFFFu;
        if (hardware_.is_cdrom(physical)) {
            return hardware_.read_cdrom(physical) |
                   (static_cast<std::uint32_t>(
                        hardware_.read_cdrom(physical + 1))
                    << 8);
        }
        if (physical <= kRamMask) return ram_.read16(physical);
        if (physical >= kScratchpadBase &&
            physical <= kScratchpadBase + kScratchpadMask) {
            const auto index = physical & kScratchpadMask & ~1u;
            return static_cast<std::uint32_t>(scratchpad_[index]) |
                   (static_cast<std::uint32_t>(scratchpad_[index + 1])
                    << 8);
        }
        if (physical >= kHardwareBase && physical < kHardwareEnd) {
            if (physical >= hardware::kSpuBase &&
                physical < hardware::kSpuEnd) {
                return read32(physical) & 0xFFFFu;
            }
            return read32(physical & ~3u) >> ((physical & 2u) * 8u);
        }
        return 0;
    }

    std::uint32_t read32(std::uint32_t address) {
        const auto physical = address & 0x1FFFFFFFu;
        if (hardware_.is_cdrom(physical)) {
            std::uint32_t result = 0;
            for (std::uint32_t index = 0; index < 4; ++index) {
                if (hardware_.is_cdrom(physical + index)) {
                    result |= static_cast<std::uint32_t>(
                                  hardware_.read_cdrom(physical + index))
                              << (index * 8);
                }
            }
            return result;
        }
        if (physical <= kRamMask) return ram_.read32(physical);
        if (physical >= kScratchpadBase &&
            physical <= kScratchpadBase + kScratchpadMask) {
            const auto index = physical & kScratchpadMask & ~3u;
            return static_cast<std::uint32_t>(scratchpad_[index]) |
                   (static_cast<std::uint32_t>(scratchpad_[index + 1])
                    << 8) |
                   (static_cast<std::uint32_t>(scratchpad_[index + 2])
                    << 16) |
                   (static_cast<std::uint32_t>(scratchpad_[index + 3])
                    << 24);
        }
        if (physical >= kHardwareBase && physical < kHardwareEnd) {
            return hardware_.read32(physical);
        }
        return 0;
    }

    void write8(std::uint32_t address, std::uint32_t value) {
        const auto physical = address & 0x1FFFFFFFu;
        if (hardware_.is_cdrom(physical)) {
            hardware_.write_cdrom(physical,
                                  static_cast<std::uint8_t>(value));
            return;
        }
        if (physical <= kRamMask) {
            ram_.write8(physical, static_cast<std::uint8_t>(value));
            return;
        }
        if (physical >= kScratchpadBase &&
            physical <= kScratchpadBase + kScratchpadMask) {
            scratchpad_[physical & kScratchpadMask] =
                static_cast<std::uint8_t>(value);
        }
    }

    void write16(std::uint32_t address, std::uint32_t value) {
        const auto physical = address & 0x1FFFFFFFu;
        if (hardware_.is_cdrom(physical)) {
            hardware_.write_cdrom(physical,
                                  static_cast<std::uint8_t>(value));
            if (hardware_.is_cdrom(physical + 1)) {
                hardware_.write_cdrom(
                    physical + 1, static_cast<std::uint8_t>(value >> 8));
            }
            return;
        }
        if (physical <= kRamMask) {
            ram_.write16(physical, static_cast<std::uint16_t>(value));
            return;
        }
        if (physical >= kScratchpadBase &&
            physical <= kScratchpadBase + kScratchpadMask) {
            const auto index = physical & kScratchpadMask & ~1u;
            scratchpad_[index] = static_cast<std::uint8_t>(value);
            scratchpad_[index + 1] = static_cast<std::uint8_t>(value >> 8);
            return;
        }
        if (physical >= hardware::kSpuBase &&
            physical < hardware::kSpuEnd) {
            write32(physical, value & 0xFFFFu);
            return;
        }
        const auto aligned = physical & ~3u;
        const auto shift = (physical & 3u) * 8u;
        const auto current = hardware_.read32(aligned);
        const auto mask = 0xFFFFu << shift;
        write32(aligned, (current & ~mask) | ((value & 0xFFFFu) << shift));
    }

    std::uint32_t gte_read_data(std::uint32_t reg) {
        return gte_.read_data(reg);
    }
    std::uint32_t gte_read_control(std::uint32_t reg) {
        return gte_.read_control(reg);
    }
    void gte_write_data(std::uint32_t reg, std::uint32_t value) {
        gte_.write_data(reg, value);
    }
    void gte_write_control(std::uint32_t reg, std::uint32_t value) {
        gte_.write_control(reg, value);
    }
    void gte_execute(std::uint32_t instruction) {
        gte_.execute(instruction);
    }
    void advance_cycles(std::uint64_t count) {
        hardware_.advance_cycles(count);
    }

    void write32(std::uint32_t address, std::uint32_t value) {
        const auto physical = address & 0x1FFFFFFFu;
        if (hardware_.is_cdrom(physical)) {
            for (std::uint32_t index = 0; index < 4; ++index) {
                if (hardware_.is_cdrom(physical + index)) {
                    hardware_.write_cdrom(
                        physical + index,
                        static_cast<std::uint8_t>(value >> (index * 8)));
                }
            }
            return;
        }
        if (physical <= kRamMask) {
            ram_.write32(physical, value);
            return;
        }
        if (physical >= kScratchpadBase &&
            physical <= kScratchpadBase + kScratchpadMask) {
            const auto index = physical & kScratchpadMask & ~3u;
            scratchpad_[index] = static_cast<std::uint8_t>(value);
            scratchpad_[index + 1] = static_cast<std::uint8_t>(value >> 8);
            scratchpad_[index + 2] = static_cast<std::uint8_t>(value >> 16);
            scratchpad_[index + 3] = static_cast<std::uint8_t>(value >> 24);
            return;
        }
        if (physical >= kHardwareBase && physical < kHardwareEnd) {
            hardware_.write32(physical, value);
        }
    }

private:
    static constexpr std::uint32_t kRamMask = 0x1FFFFFu;
    static constexpr std::uint32_t kScratchpadBase = 0x1F800000u;
    static constexpr std::uint32_t kScratchpadMask = 0x3FFu;
    static constexpr std::uint32_t kHardwareBase = 0x1F801000u;
    static constexpr std::uint32_t kHardwareEnd = 0x1F803000u;

    Memory& ram_;
    HardwareMap hardware_;
    Gte gte_;
    std::array<std::uint8_t, 0x400> scratchpad_{};
};

[[noreturn]] inline void unsupported_instruction(
    const CpuState& state,
    std::uint32_t address,
    std::uint32_t instruction) {
    (void)state;
    char message[128];
    std::snprintf(
        message,
        sizeof(message),
        "generated instruction not implemented at 0x%08X: 0x%08X",
        address,
        instruction);
    throw std::runtime_error(message);
}

}  // namespace r6
