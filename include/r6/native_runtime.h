#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "r6/cpu_state.h"
#include "r6/memory.h"

namespace r6 {

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
