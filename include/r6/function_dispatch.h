#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>

#include "r6/cpu_state.h"
#include "r6/memory.h"
#include "r6/platform_services.h"

namespace r6 {

using TranslatedFunction = std::uint32_t (*)(
    CpuState&, Memory&, PlatformServices&);

class FunctionDispatcher {
public:
    void register_function(std::uint32_t address, TranslatedFunction function) {
        functions_[address] = function;
    }

    bool contains(std::uint32_t address) const {
        return functions_.find(address) != functions_.end();
    }

    std::uint32_t step(
        CpuState& cpu,
        Memory& memory,
        PlatformServices& services) const {
        const auto found = functions_.find(cpu.pc);
        if (found == functions_.end()) {
            throw std::out_of_range("no translated function registered for PC");
        }
        return found->second(cpu, memory, services);
    }

    void run(
        CpuState& cpu,
        Memory& memory,
        PlatformServices& services,
        std::size_t max_steps) const {
        for (std::size_t step_count = 0; step_count < max_steps; ++step_count) {
            cpu.pc = step(cpu, memory, services);
        }
        throw std::runtime_error("native translation step budget exhausted");
    }

private:
    std::unordered_map<std::uint32_t, TranslatedFunction> functions_;
};

}  // namespace r6
