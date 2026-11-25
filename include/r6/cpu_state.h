#pragma once

#include <cstdint>

namespace r6 {

struct CpuState {
    std::uint32_t gpr[32]{};
    std::uint32_t hi{};
    std::uint32_t lo{};
    std::uint32_t pc{};
    // COP0 is kept with the register file so translated mfc0/mtc0 and the
    // per-block oracle start from the same status/EPC/Cause state.
    std::uint32_t cop0[32]{};
};

}  // namespace r6
