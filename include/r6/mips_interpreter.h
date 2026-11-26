#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "r6/cpu_state.h"
#include "r6/event_trace.h"
#include "r6/gte.h"
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
    // General exception vector. Real hardware selects 0xA0000080 when SR.BEV is
    // clear, which is BIOS code this project does not carry.
    static constexpr std::uint32_t kExceptionVector = 0x80000080u;

    MipsInterpreter(Memory& ram, PlatformServices& services);

    CpuState& cpu() { return cpu_; }
    const CpuState& cpu() const { return cpu_; }

    RunResult run(std::uint64_t max_steps);
    // One differential step: unlike run(1), stop immediately after taking an
    // interrupt instead of executing the first handler instruction. This lets
    // an event-aware native block and the oracle meet at the same PC.
    RunResult run_event_step();

    std::uint64_t gte_commands() const { return gte_commands_; }
    std::uint64_t gte_unsupported_commands() const {
        return gte_.unsupported();
    }
    Gte& gte() { return gte_; }
    const Gte& gte() const { return gte_; }
    std::uint64_t dma_writes() const { return dma_writes_; }
    std::uint64_t dma_lists() const { return dma_lists_; }
    std::uint64_t dma_words() const { return dma_words_; }
    std::uint64_t vblanks() const { return vblank_count_; }
    std::uint64_t vblank_polls() const { return vblank_polls_; }
    std::uint64_t vblank_next() const { return vblank_next_; }
    std::uint64_t interrupts() const { return interrupts_delivered_; }
    std::uint32_t cop0_status() { return cop0_[12]; }
    std::uint32_t peek(std::uint32_t address) { return read32(address); }
    void watch_pc(std::uint32_t address) { watched_.emplace_back(address, 0); }
    void watch_args(std::uint32_t address) { watch_args_.push_back(address); }
    const std::vector<std::string>& watched_args() const {
        return watched_args_log_;
    }
    void set_hot_enabled(bool enabled) { hot_enabled_ = enabled; }
    void set_trace_reg(std::uint32_t address) { trace_reg_ = address; }
    // Logs every store that lands on one address, with the writing PC, so a
    // value that appears in guest memory can be traced back to its writer.
    void watch_write(std::uint32_t address) { watch_writes_.push_back(address); }
    const std::vector<std::string>& write_log() const { return write_log_; }
    // Stores into the first words of the exception vector, with the writing
    // PC, so a run shows whether the title installs a handler there.
    struct VectorWrite {
        std::uint32_t address{0};
        std::uint32_t value{0};
        std::uint32_t pc{0};
        int width{32};
    };
    const std::vector<VectorWrite>& vector_writes() const {
        return vector_writes_;
    }
    void set_find_value(std::uint32_t value) { find_value_ = value; }
    const std::vector<std::string>& trace_log() const { return trace_log_; }
    const std::vector<std::string>& dma3_log() const { return dma3_log_; }
    const std::map<std::uint32_t, std::uint64_t>& hot() const { return hot_; }
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& watched() const {
        return watched_;
    }
    const std::vector<std::uint32_t>& irq_pcs() const { return irq_pcs_; }
    const std::vector<std::string>& interrupt_log() const {
        return interrupt_log_;
    }
    const EventTrace& events() const { return events_; }
    const std::vector<std::vector<std::uint32_t>>& irq_traces() const {
        return irq_traces_;
    }
    // Tail of an interrupt handler that exhausted its step budget, so an
    // overrun can be attributed to the loop it actually reached.
    const std::vector<std::vector<std::uint32_t>>& irq_overrun_traces() const {
        return irq_overrun_traces_;
    }
    // Busiest PCs at the moment a handler exhausted its budget. The tail alone
    // shows only the last instructions, which can be a plain subroutine that
    // is not where the time went.
    const std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>>&
    irq_overrun_hot() const {
        return irq_overrun_hot_;
    }
    const std::vector<std::string>& irq_overrun_log() const {
        return irq_overrun_log_;
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
    // Interrupts left pending because nothing was installed at the vector.
    std::uint64_t missing_vectors() const { return missing_vectors_; }
    // COP0 register file, for diagnostics and tests (EPC is r14, Cause r13).
    std::uint32_t cop0(std::size_t index) const {
        return index < cop0_.size() ? cop0_[index] : 0;
    }
    // The differential harness starts each oracle block from the native
    // checkpoint's COP0 file.
    void set_cop0(const std::uint32_t* values) {
        for (std::size_t index = 0; index < cop0_.size(); ++index) {
            cop0_[index] = values[index];
        }
    }
    // Delivering vertical-blank interrupts is opt-in: the guest handler needs
    // devices that are still stubs, so it can park inside the handler.
    void set_vblank_enabled(bool enabled) { vblank_enabled_ = enabled; }
    // Handler budget for the overrun guard. The CD dispatcher drains sectors
    // inside its own loop, so too small a budget unwinds it mid-read; tests
    // that check the guard pin their own value here.
    void set_interrupt_budget(std::uint64_t steps) {
        interrupt_step_budget_ = steps;
    }
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
        // Set by RFE: the exception handler has finished its critical section.
        bool returned_from_exception{false};
    };

    StepResult execute(std::uint32_t instruction, std::uint32_t pc);
    bool service_bios_call();
    void record_call_site(std::uint32_t pc);
    // Walks a DMA2 linked list and feeds its packets to the GPU.
    void run_dma2_list();
    // Copies a CD-ROM sector into main RAM for DMA channel 3.
    void run_dma3_transfer();
    // Copies main RAM into the SPU for DMA channel 4 and signals completion.
    void run_dma4_transfer();
    // Raises the vertical-blank interrupt.
    void poll_vblank();
    void note_cdrom_poll(std::uint32_t physical);
    void note_watched_write(std::uint32_t address, std::uint32_t value,
                            int width);
    // Records a store into the exception vector with the PC that made it.
    void note_vector_write(std::uint32_t address, std::uint32_t value,
                           int width);
    // Takes the general exception at 0x80000080 when an unmasked interrupt is
    // pending: EPC keeps the interrupted PC, CAUSE gets ExcCode 0 and IP2, and
    // SR pushes its mode bits with IEc cleared. The guest handler returns with
    // RFE and `jr $k0`, as on hardware.
    void deliver_interrupt();
    // Pops the SR status stack, the way RFE does.
    void return_from_exception();
    // Saves the register file at exception entry and restores it when the
    // handler returns through B(17h), as the kernel's exception frame does.
    // Without it the guest resumes with the handler's stack and callee-saved
    // registers, and the next function return jumps through a stale RA.
    void save_interrupt_context();
    void restore_interrupt_context();
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
    Gte gte_;
    // Every guest write to COP0 SR, as (pc, value), so interrupt-enable
    // transitions during bring-up are visible.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sr_writes_;

    // First PCs executed inside the last delivered interrupt routine.
    std::vector<std::uint32_t> irq_pcs_;
    std::vector<std::vector<std::uint32_t>> irq_traces_;
    // Last instructions executed inside a handler, kept as a ring while it
    // runs. Copied into irq_overrun_traces_ when the budget expires.
    std::array<std::uint32_t, 64> irq_tail_{};
    std::size_t irq_tail_size_{0};
    std::size_t irq_tail_next_{0};
    std::vector<std::vector<std::uint32_t>> irq_overrun_traces_;
    std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>>
        irq_overrun_hot_;
    std::vector<std::string> irq_overrun_log_;
    // PCs executed inside the last handler, with how often, so a stall can
    // be attributed to a single loop instead of guessed at.
    std::map<std::uint32_t, std::uint64_t> irq_hot_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> watched_;
    std::map<std::uint32_t, std::uint64_t> hot_;
    bool hot_enabled_{false};
    std::uint32_t trace_reg_{0};
    std::uint32_t find_value_{0};
    std::vector<std::uint32_t> watch_writes_;
    std::vector<std::string> write_log_;
    std::vector<std::string> trace_log_;
    std::vector<std::string> dma3_log_;
    std::vector<std::uint32_t> watch_args_;
    std::vector<std::string> watched_args_log_;
    bool bad_heap_logged_ = false;
    std::vector<std::uint32_t> cdrom_polls_;
    std::vector<std::string> interrupt_log_;
    EventTrace events_;

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
    std::uint64_t vblank_next_{0};
    std::uint64_t vblank_polls_{0};
    bool vblank_enabled_{false};
    // Pending branch-delay state, kept across run() calls so the reference
    // interpreter can be single-stepped for differential checks.
    bool in_delay_slot_{false};
    std::uint32_t delay_target_{0};
    bool stop_after_event_{false};
    bool in_interrupt_{false};
    // Libetc critical sections nest. Keep the interrupt-enable bit from the
    // outermost entry so an inner exit does not restore interrupts early.
    std::uint32_t critical_section_depth_{0};
    bool critical_section_ie_{false};
    std::uint64_t interrupt_steps_{0};
    // Long enough for the CD streaming dispatcher, short enough to catch a
    // handler that never returns.
    std::uint64_t interrupt_step_budget_{2000000u};
    // SR before the exception push, kept so an abandoned handler can be unwound
    // without leaving interrupts disabled.
    std::uint32_t interrupt_saved_sr_{0};
    std::array<std::uint32_t, 32> interrupt_gpr_{};
    std::uint32_t interrupt_lo_{0};
    std::uint32_t interrupt_hi_{0};
    bool interrupt_context_valid_{false};
    std::uint64_t missing_vectors_{0};
    std::vector<VectorWrite> vector_writes_;

public:
    const std::vector<std::uint32_t>& cdrom_polls() const {
        return cdrom_polls_;
    }

    const std::vector<std::string>& cdrom_log() { return hardware_.cdrom().log(); }
    const std::vector<std::string>& cdrom_register_log() {
        return hardware_.cdrom().register_log();
    }

    HardwareMap& hardware() { return hardware_; }
    const std::vector<std::uint8_t>& scratchpad() const {
        return scratchpad_;
    }

private:
    std::uint64_t unknown_hardware_reads_{0};
    std::uint64_t unknown_hardware_writes_{0};
    std::map<std::uint32_t, std::uint64_t> hardware_reads_;
    std::map<std::uint32_t, std::uint64_t> hardware_writes_;
};

}  // namespace r6
