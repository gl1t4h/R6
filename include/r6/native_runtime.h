#pragma once

#include <cstdint>

#include "r6/cpu_state.h"
#include "r6/memory.h"
#include "r6/platform_services.h"

namespace r6 {

[[noreturn]] inline void unsupported_instruction(
    const CpuState& state,
    std::uint32_t address,
    std::uint32_t instruction) {
    (void)state;
    (void)address;
    (void)instruction;
    throw "R6 generated instruction is not implemented yet";
}

}  // namespace r6
