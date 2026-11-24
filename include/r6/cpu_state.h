#pragma once

#include <cstdint>

namespace r6 {

struct CpuState {
    std::uint32_t gpr[32]{};
    std::uint32_t hi{};
    std::uint32_t lo{};
    std::uint32_t pc{};
};

}  // namespace r6
