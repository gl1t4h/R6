#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "r6/cpu_state.h"
#include "r6/hardware_map.h"
#include "r6/memory.h"
#include "r6/platform_services.h"

namespace r6 {

enum class StopReason {
    step_budget,
    returned,
    unsupported_instruction,
    bios_exit,
    // The guest executed BREAK. PsyQ startup code ends with BREAK after main
    // returns, so this is a normal program exit, not a fault.
    guest_break,
};

struct RunResult {
    std::uint64_t steps{0};
    StopReason reason{StopReason::step_budget};
    std::uint32_t pc{0};
    std::uint32_t instruction{0};
};

// Development runner that executes the user's own executable instruction by
// instruction using the native platform services.
//
// This exists so the recompilation pipeline has a reference execution to
// compare against; it is not a shipping emulator, and it does not attempt
// cycle accuracy, the CD-ROM controller beyond sector reads, or the SPU.
class MipsInterpreter {
public:
    MipsInterpreter(Memory& ram, PlatformServices& services);

    CpuState& cpu() { return cpu_; }
    const CpuState& cpu() const { return cpu_; }

    RunResult run(std::uint64_t max_steps);

    std::uint64_t gte_commands() const { return gte_commands_; }
    std::uint64_t dma_writes() const { return dma_writes_; }
    std::uint64_t unknown_hardware_reads() const { return unknown_hardware_reads_; }
    std::uint64_t unknown_hardware_writes() const {
        return unknown_hardware_writes_;
    }
    const std::vector<std::uint32_t>& recent_pcs() const { return recent_pcs_; }
    // Hardware register address -> access count, for finding unimplemented
    // devices during bring-up.
    const std::map<std::uint32_t, std::uint64_t>& hardware_reads() const {
        return hardware_reads_;
    }
    const std::map<std::uint32_t, std::uint64_t>& hardware_writes() const {
        return hardware_writes_;
    }

private:
    struct StepResult {
        bool branch{false};
        std::uint32_t target{0};
        bool unsupported{false};
        bool halted{false};
    };

    StepResult execute(std::uint32_t instruction, std::uint32_t pc);
    bool service_bios_call();

    std::uint32_t read8(std::uint32_t address);
    std::uint32_t read16(std::uint32_t address);
    std::uint32_t read32(std::uint32_t address);
    void write8(std::uint32_t address, std::uint32_t value);
    void write16(std::uint32_t address, std::uint32_t value);
    void write32(std::uint32_t address, std::uint32_t value);

    Memory& ram_;
    PlatformServices& services_;
    HardwareMap hardware_;
    CpuState cpu_{};
    std::vector<std::uint8_t> scratchpad_;
    std::vector<std::uint32_t> cop0_;
    std::vector<std::uint32_t> gte_data_;
    std::vector<std::uint32_t> recent_pcs_;

    std::uint64_t gte_commands_{0};
    std::uint64_t dma_writes_{0};
    std::uint64_t unknown_hardware_reads_{0};
    std::uint64_t unknown_hardware_writes_{0};
    std::map<std::uint32_t, std::uint64_t> hardware_reads_;
    std::map<std::uint32_t, std::uint64_t> hardware_writes_;
};

}  // namespace r6
