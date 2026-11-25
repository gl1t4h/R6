#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
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
    // A delivered interrupt routine never returned to its caller.
    interrupt_stalled,
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
    std::uint64_t dma_lists() const { return dma_lists_; }
    std::uint64_t dma_words() const { return dma_words_; }
    std::uint64_t vblanks() const { return vblank_count_; }
    std::uint64_t interrupts() const { return interrupts_delivered_; }
    std::uint32_t cop0_status() { return cop0_[12]; }
    std::uint32_t peek(std::uint32_t address) { return read32(address); }
    void watch_pc(std::uint32_t address) { watched_.emplace_back(address, 0); }
    void set_hot_enabled(bool enabled) { hot_enabled_ = enabled; }
    void set_trace_reg(std::uint32_t address) { trace_reg_ = address; }
    void set_find_value(std::uint32_t value) { find_value_ = value; }
    const std::vector<std::string>& trace_log() const { return trace_log_; }
    const std::map<std::uint32_t, std::uint64_t>& hot() const { return hot_; }
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& watched() const {
        return watched_;
    }
    const std::vector<std::uint32_t>& irq_pcs() const { return irq_pcs_; }
    const std::vector<std::string>& interrupt_log() const {
        return interrupt_log_;
    }
    const std::vector<std::vector<std::uint32_t>>& irq_traces() const {
        return irq_traces_;
    }
    const std::map<std::uint32_t, std::uint64_t>& irq_hot() const {
        return irq_hot_;
    }
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& sr_writes()
        const {
        return sr_writes_;
    }
    std::uint32_t irq_status() { return hardware_.interrupt_status(); }
    std::uint32_t irq_mask() { return hardware_.interrupt_mask(); }
    std::uint32_t cd_sectors_read() { return hardware_.cdrom().sectors_read(); }
    std::uint64_t interrupt_overruns() const { return interrupt_overruns_; }
    // Delivering vertical-blank interrupts is opt-in: the guest handler needs
    // devices that are still stubs, so it can park inside the handler.
    void set_vblank_enabled(bool enabled) { vblank_enabled_ = enabled; }
    std::uint64_t unknown_hardware_reads() const { return unknown_hardware_reads_; }
    std::uint64_t unknown_hardware_writes() const {
        return unknown_hardware_writes_;
    }
    const std::vector<std::uint32_t>& recent_pcs() const { return recent_pcs_; }
    // Call sites of the most recent calls, so a stop can be traced back to its
    // caller chain.
    const std::vector<std::uint32_t>& recent_calls() const {
        return recent_calls_;
    }
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
    void record_call_site(std::uint32_t pc);
    // Walks a DMA2 linked list and feeds its packets to the GPU.
    void run_dma2_list();
    // Copies a CD-ROM sector into main RAM for DMA channel 3.
    void run_dma3_transfer();
    // Raises the vertical-blank interrupt.
    void poll_vblank();
    void enter_entry_int();
    // Enters the queued guest interrupt routine when a pending interrupt is
    // unmasked, which stands in for the retail kernel's dispatcher.
    void deliver_interrupt();
    static std::string format_hex(std::uint32_t value);

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
    // Every guest write to COP0 SR, as (pc, value), so interrupt-enable
    // transitions during bring-up are visible.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sr_writes_;

    // First PCs executed inside the last delivered interrupt routine.
    std::vector<std::uint32_t> irq_pcs_;
    std::vector<std::vector<std::uint32_t>> irq_traces_;
    // PCs executed inside the last handler, with how often, so a stall can
    // be attributed to a single loop instead of guessed at.
    std::map<std::uint32_t, std::uint64_t> irq_hot_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> watched_;
    std::map<std::uint32_t, std::uint64_t> hot_;
    bool hot_enabled_{false};
    std::uint32_t trace_reg_{0};
    std::uint32_t find_value_{0};
    std::vector<std::string> trace_log_;
    std::vector<std::string> interrupt_log_;

    std::vector<std::uint32_t> recent_pcs_;
    std::vector<std::uint32_t> recent_calls_;

    std::uint64_t gte_commands_{0};
    std::uint64_t dma_writes_{0};
    std::uint64_t dma_lists_{0};
    std::uint64_t dma_words_{0};
    std::uint64_t vblank_count_{0};
    std::uint64_t interrupts_delivered_{0};
    bool interrupts_enabled_{true};
    std::uint64_t interrupt_overruns_{0};
    std::uint32_t vblank_next_{0};
    bool vblank_enabled_{false};
    bool in_interrupt_{false};
    std::uint64_t interrupt_steps_{0};
    std::uint32_t interrupt_return_pc_{0};
    std::uint32_t interrupt_saved_ra_{0};
    // The kernel saves the interrupted context before entering a handler, so
    // entering one here must not leave the handler's registers behind.
    std::array<std::uint32_t, 32> interrupt_saved_gpr_{};
    std::uint32_t interrupt_saved_hi_{0};
    std::uint32_t interrupt_saved_lo_{0};
    std::uint32_t interrupt_saved_cause_{0};
    // Pending I_STAT bits taken when the handler was entered; acknowledged
    // when it returns, the way the kernel does.
    std::uint32_t interrupt_pending_bits_{0};
    // Every queued SysEnqIntRP routine is entered for one interrupt, the way
    // the kernel walks the priority list.
    std::vector<std::uint32_t> interrupt_routines_;
    std::size_t interrupt_routine_index_{0};
    bool interrupt_hook_done_{false};
    std::uint64_t interrupt_hooks_{0};

public:
    std::uint64_t hooks() const { return interrupt_hooks_; }

    const std::vector<std::string>& cdrom_log() { return hardware_.cdrom().log(); }

private:
    std::uint64_t unknown_hardware_reads_{0};
    std::uint64_t unknown_hardware_writes_{0};
    std::map<std::uint32_t, std::uint64_t> hardware_reads_;
    std::map<std::uint32_t, std::uint64_t> hardware_writes_;
};

}  // namespace r6
