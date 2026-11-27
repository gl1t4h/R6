#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

#include "r6/unaligned.h"
#include "r6/cpu_state.h"
#include "r6/event_trace.h"
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
        : ram_(ram), services_(services), hardware_(services) {
        // Match the interpreter's kernel hand-off: the disc controller asserts
        // its interrupt line once its enable bit has been programmed.
        hardware_.set_cdrom_interrupt_enabled(true);
    }

    // The differential runner uses this to prove that a translated block
    // actually reads a specific guest address, not merely that it is present
    // in the image. A zero address disables the watch.
    static void set_watch_address(std::uint32_t address) {
        watch_address_ = address;
        watch_hits_ = 0;
    }
    static std::uint64_t watch_hits() { return watch_hits_; }

    std::uint32_t read8(std::uint32_t address) {
        note_watch(address, "read8");
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
        note_watch(address, "read16");
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

    // Directory-table watch: records every access to the parsed directory
    // range together with the block that made it, so an early read can be
    // told apart from a late parse.
    static void set_trace_pc(std::uint32_t pc) { trace_pc_ = pc; }
    static void trace_table(char kind, std::uint32_t address) {
        static const bool enabled = std::getenv("R6_TRACE_TABLE") != nullptr;
        if (!enabled) return;
        if (address < 0x800BF6E0u || address > 0x800BF760u) return;
        std::fprintf(stderr, "TBL %c %08X pc=%08X\n", kind, address, trace_pc_);
    }
    // Write-value watch: names the block and the value for stores into the
    // extent entry that feeds the loader's read.
    static void trace_table_write(std::uint32_t address, std::uint32_t value) {
        static const bool enabled = std::getenv("R6_TRACE_TABLE") != nullptr;
        if (!enabled) return;
        if (address != 0x800BF6E8u) return;
        std::fprintf(stderr, "TBLW %08X = %08X pc=%08X\n", address, value,
                     trace_pc_);
    }
    inline static std::uint32_t trace_pc_ = 0;

    // Value watch: reports the block that loads a specific value, which is how
    // a wrong operand gets tied to the load that produced it.
    static void trace_value(std::uint32_t address, std::uint32_t value) {
        static const char* const mode = std::getenv("R6_TRACE_VALUE");
        if (mode == nullptr) return;
        if (std::strcmp(mode, "all") == 0) {
            if (address < 0x800BE000u || address > 0x800C0000u) return;
        } else if (value != 0x000C0016u) {
            return;
        }
        std::fprintf(stderr, "VALR %08X = %08X pc=%08X\n", address, value,
                     trace_pc_);
    }

    std::uint32_t read32(std::uint32_t address) {
        const auto value = read32_inner(address);
        trace_value(address, value);
        return value;
    }

    std::uint32_t read32_inner(std::uint32_t address) {
        note_watch(address, "read32");
        trace_table('R', address);
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
        note_watch(address, "write8");
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
        note_watch(address, "write16");
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
            hardware_.write_spu16(physical, value & 0xFFFFu);
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
    // One guest instruction's worth of clock. Generated blocks call this
    // so the native path keeps the interpreter's timing, which is what
    // makes the per-block differential compare equal cycles.
    void advance_instruction() {
        hardware_.advance_cycles(kCyclesPerInstruction);
    }

    // Generated blocks call this at the start of every instruction. It mirrors
    // the interpreter's single-step event poll: pad/vblank ticks happen before
    // the instruction, and a pending unmasked interrupt replaces the PC. A
    // delay slot calls with allow_interrupt=false, matching the interpreter's
    // rule that a branch and its delay slot are atomic for interrupt entry.
    bool event_tick(CpuState& cpu, bool allow_interrupt = true) {
        if (vblank_enabled_) poll_vblank();
        if (hardware_.pad_tick()) events_.record("pad");
        if (in_interrupt_) {
            // Match the interpreter's handler-budget unwind: a handler that
            // never returns is dropped back to EPC instead of blocking the
            // interrupted stream forever.
            if (++interrupt_steps_ > kInterruptStepBudget) {
                cpu.pc = cpu.cop0[14];
                cpu.cop0[12] = interrupt_saved_sr_;
                restore_interrupt_context(cpu);
                in_interrupt_ = false;
                if (++interrupt_overruns_ >= kMaxInterruptOverruns) {
                    interrupts_enabled_ = false;
                }
                // Abort the current block so the next dispatch starts at EPC.
                return true;
            }
            return false;
        }
        if (!allow_interrupt) return false;
        // Every instruction has its own dispatch entry (the translator emits
        // one for each interior instruction of a block), so an interrupt taken
        // mid-block can set EPC to the instruction that did not run and the
        // dispatcher resumes exactly there. block_boundary_interrupts_ is the
        // older coarse mode, kept for runs that need a single entry per block.
        if (block_boundary_interrupts_) return false;
        return deliver_interrupt(cpu);
    }

    // Opt-in for continuous runs: take any pending interrupt now, which is a
    // block boundary, instead of inside a block. Latency is at most one block.
    void set_block_boundary_interrupts(bool enabled) {
        block_boundary_interrupts_ = enabled;
    }
    bool service_interrupt(CpuState& cpu) {
        if (!block_boundary_interrupts_ || in_interrupt_) return false;
        return deliver_interrupt(cpu);
    }

    void set_vblank_enabled(bool enabled) { vblank_enabled_ = enabled; }
    bool in_interrupt() const { return in_interrupt_; }
    const EventTrace& events() const { return events_; }
    std::uint64_t interrupts() const { return interrupts_delivered_; }
    std::uint64_t interrupt_overruns() const { return interrupt_overruns_; }
    bool interrupts_enabled() const { return interrupts_enabled_; }
    std::uint64_t missing_vectors() const { return missing_vectors_; }

    // Syscall is libetc's critical-section API, not a no-op: the retail
    // kernel masks interrupts for Syscall(1) and restores them for
    // Syscall(2), and the guest relies on both around driver sections that
    // are not reentrant. The interpreter does the same, so the generated
    // blocks must too or the two runs diverge on SR (IEp).
    void syscall(CpuState& cpu, std::uint32_t code) {
        if (code == 1) {
            cpu.gpr[2] = (cpu.cop0[12] & 1u) != 0 ? 1u : 0u;
            cpu.cop0[12] &= ~0x405u;
        } else if (code == 2) {
            cpu.cop0[12] |= 0x405u;
        }
    }

    // RFE: pop the SR mode stack. The generated block advances the PC itself.
    void rfe(CpuState& cpu) {
        const auto status = cpu.cop0[12];
        cpu.cop0[12] = (status & ~0x3Fu) | ((status >> 2) & 0xFu);
        in_interrupt_ = false;
        events_.record("rfe");
    }

    // B(17h) ReturnFromException ends the exception and resumes at EPC.
    bool take_bios_return(CpuState& cpu) {
        if (!services_.take_return_from_exception()) return false;
        if (!in_interrupt_) return false;
        finish_exception(cpu);
        return true;
    }

    HardwareMap& hardware() { return hardware_; }
    const HardwareMap& hardware() const { return hardware_; }
    Gte& gte() { return gte_; }
    const Gte& gte() const { return gte_; }
    const std::array<std::uint8_t, 0x400>& scratchpad() const {
        return scratchpad_;
    }

    void write32(std::uint32_t address, std::uint32_t value) {
        note_watch(address, "write32");
        trace_table('W', address);
        trace_table_write(address, value);
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
            if (physical == hardware::kDma2ChannelControl) {
                const auto start = (value & (1u << 24)) != 0;
                const auto sync_mode = (value >> 9) & 3u;
                const auto from_ram = (value & 1u) != 0;
                if (start && sync_mode == 2 && from_ram) run_dma2_list();
            }
            if (physical == hardware::kMdecData ||
                physical == hardware::kMdecStatus) {
                pump_mdec_dma();
            }
            if (physical == hardware::kDma0ChannelControl) {
                const auto start = (value & (1u << 24)) != 0;
                const auto to_device = (value & 1u) != 0;
                if (start && to_device) {
                    run_dma0_transfer();
                    pump_mdec_dma();
                }
            }
            if (physical == hardware::kDma1ChannelControl) {
                const auto start = (value & (1u << 24)) != 0;
                const auto to_ram = (value & 1u) == 0;
                if (start && to_ram) run_dma1_transfer();
            }
            if (physical == hardware::kDma3ChannelControl) {
                const auto start = (value & (1u << 24)) != 0;
                const auto to_ram = (value & 1u) == 0;
                if (std::getenv("R6_TRACE_DMA3") != nullptr) {
                    // The guest programming channel 3 is the only way a
                    // transfer can happen, so this line separates "the guest
                    // never asked" from "we were asked and did nothing".
                    std::fprintf(stderr, "D3W %llu chcr=%08X start=%u to_ram=%u\n",
                                 static_cast<unsigned long long>(hardware_.cycles()), value,
                                 start ? 1u : 0u, to_ram ? 1u : 0u);
                }
                if (start && to_ram) run_dma3_transfer();
            }
            if (physical == hardware::kDma4ChannelControl) {
                const auto start = (value & (1u << 24)) != 0;
                const auto to_device = (value & 1u) != 0;
                if (start && to_device) run_dma4_transfer();
            }
        }
    }

private:
    void poll_vblank() {
        constexpr std::uint32_t kVblankBit = 1u << 0;
        constexpr std::uint64_t kVblankPeriodCycles = 564480u;

        ++vblank_polls_;
        const auto now = hardware_.cycles();
        if (vblank_next_ == 0) {
            vblank_next_ = now + kVblankPeriodCycles;
            return;
        }
        if (now < vblank_next_) return;
        vblank_next_ = now + kVblankPeriodCycles;
        ++vblank_count_;
        hardware_.toggle_gpu_field();
        hardware_.raise_interrupt(kVblankBit);
        events_.record("vblank");
    }

    bool deliver_interrupt(CpuState& cpu) {
        if (!interrupts_enabled_) return false;
        const auto status = cpu.cop0[12];
        if ((status & 0x1u) == 0 || (status & 0x400u) == 0) return false;
        const auto pending = hardware_.interrupt_status() &
                             hardware_.interrupt_mask();
        if (pending == 0) return false;

        static constexpr std::uint32_t kExceptionVector = 0x80000080u;
        const auto vector = ram_.read32(kExceptionVector & kRamMask);
        const auto entry_int = services_.entry_int();
        const auto entry_pc = entry_int != 0
                                  ? ram_.read32(entry_int & kRamMask)
                                  : 0;
        if (vector == 0 && entry_pc == 0) {
            ++missing_vectors_;
            char event[96];
            std::snprintf(event, sizeof(event),
                          "missing_vector pending=0x%08X pc=0x%08X", pending,
                          cpu.pc);
            events_.record(event);
            return false;
        }

        interrupt_saved_sr_ = status;
        cpu.cop0[14] = cpu.pc;
        cpu.cop0[13] = (cpu.cop0[13] & ~0x0000FF7Cu) | 0x00000400u;
        cpu.cop0[12] = (status & ~0x3Fu) | ((status & 0x0Fu) << 2);
        save_interrupt_context(cpu);
        if (vector != 0) {
            cpu.pc = kExceptionVector;
        } else {
            cpu.gpr[31] = ram_.read32(entry_int & kRamMask);
            cpu.gpr[29] = ram_.read32((entry_int + 4) & kRamMask);
            cpu.gpr[30] = ram_.read32((entry_int + 8) & kRamMask);
            for (std::uint32_t index = 0; index < 8; ++index) {
                cpu.gpr[16 + index] =
                    ram_.read32((entry_int + 12 + index * 4) & kRamMask);
            }
            cpu.gpr[28] = ram_.read32((entry_int + 0x2C) & kRamMask);
            cpu.gpr[2] = 1;
            cpu.pc = entry_pc;
        }
        in_interrupt_ = true;
        interrupt_steps_ = 0;
        ++interrupts_delivered_;

        char event[128];
        std::snprintf(event, sizeof(event),
                      "%s pending=0x%08X epc=0x%08X target=0x%08X",
                      vector != 0 ? "interrupt" : "entryint", pending,
                      cpu.cop0[14], cpu.pc);
        events_.record(event);
        return true;
    }

    void finish_exception(CpuState& cpu) {
        const auto status = cpu.cop0[12];
        cpu.cop0[12] = (status & ~0x3Fu) | ((status >> 2) & 0xFu);
        cpu.pc = cpu.cop0[14];
        restore_interrupt_context(cpu);
        in_interrupt_ = false;
        events_.record("bios_return");
    }

    void save_interrupt_context(CpuState& cpu) {
        for (std::size_t index = 0; index < 32; ++index) {
            interrupt_gpr_[index] = cpu.gpr[index];
        }
        interrupt_lo_ = cpu.lo;
        interrupt_hi_ = cpu.hi;
        interrupt_context_valid_ = true;
    }

    void restore_interrupt_context(CpuState& cpu) {
        if (!interrupt_context_valid_) return;
        for (std::size_t index = 0; index < 32; ++index) {
            cpu.gpr[index] = interrupt_gpr_[index];
        }
        cpu.lo = interrupt_lo_;
        cpu.hi = interrupt_hi_;
        interrupt_context_valid_ = false;
    }

    // DMA2 walks a packet list in main RAM and forwards its payload to GP0.
    void run_dma2_list() {
        constexpr std::uint32_t kEndMarker = 0x00FFFFFFu;
        constexpr std::uint32_t kRamAddressMask = 0x001FFFFCu;
        constexpr std::size_t kMaxNodes = 4096;
        constexpr std::uint64_t kMaxWords = 1u << 20;

        auto address = hardware_.dma_address(2) & kRamAddressMask;
        std::uint64_t words_sent = 0;
        for (std::size_t node = 0;
             node < kMaxNodes && words_sent < kMaxWords; ++node) {
            const auto tag = ram_.read32(address & kRamMask);
            const auto words = tag >> 24;
            const auto next = tag & kEndMarker;
            for (std::uint32_t word = 0; word < words; ++word) {
                const auto source = (address + 4 + word * 4) & kRamAddressMask;
                hardware_.write32(hardware::kGpuCommand,
                                 ram_.read32(source & kRamMask));
                ++words_sent;
            }
            if (next == kEndMarker) break;
            address = next & kRamAddressMask;
        }
    }

    void complete_mdec_dma(std::size_t channel) {
        const auto dicr = hardware_.read32(hardware::kDmaInterrupt);
        const bool interrupt = (dicr & (1u << 23)) != 0 &&
                               (dicr & (1u << (16 + channel))) != 0;
        hardware_.schedule_dma_completion(channel, 8u, interrupt);
    }

    // DMA0 request-mode transfer: the compressed stream from RAM into the
    // decoder's input fifo.
    void run_dma0_transfer() {
        const auto counter = hardware_.dma_counter(0);
        const auto blocks = counter >> 16;
        auto words = counter & 0xFFFFu;
        if (words == 0) words = 0x10000u;
        if (blocks > 1) words *= blocks;
        const auto address = 0x80000000u | hardware_.dma_address(0);
        if (std::getenv("MDEC_TRACE") != nullptr) {
            // What the player actually hands the decoder, so a stream that is
            // not the disc's payload can be seen instead of guessed at.
            static int traced = 0;
            if (traced < 8) {
                std::fprintf(stderr, "dma0 src %08X words %u\n", address, words);
                for (std::uint32_t index = 0; index < 16 && index < words;
                     ++index) {
                    std::fprintf(stderr, "  [%u] %08X\n", index,
                                 ram_.read32((address + index * 4) & kRamMask));
                }
                static bool dumped = false;
                if (words > 512 && !dumped) {  // the decode, not a table
                    dumped = true;
                    if (auto* dump = std::fopen("work/mdec_in.bin", "wb")) {
                        for (std::uint32_t index = 0; index < words; ++index) {
                            const auto word =
                                ram_.read32((address + index * 4) & kRamMask);
                            std::fwrite(&word, 4, 1, dump);
                        }
                        std::fclose(dump);
                    }
                }
                ++traced;
            }
        }
        for (std::uint32_t index = 0; index < words; ++index) {
            hardware_.mdec().push_word(
                ram_.read32((address + index * 4) & kRamMask));
        }
        complete_mdec_dma(0);
        events_.record("dma0");
    }

    // DMA1 request-mode transfer: the decoder's pixels into main RAM.
    void run_dma1_transfer() {
        const auto counter = hardware_.dma_counter(1);
        const auto blocks = counter >> 16;
        auto words = counter & 0xFFFFu;
        if (words == 0) words = 0x10000u;
        if (blocks > 1) words *= blocks;
        run_dma1_words(words);
    }

    void run_dma1_words(std::uint32_t words) {
        const auto address = 0x80000000u | hardware_.dma_address(1);
        for (std::uint32_t index = 0; index < words; ++index) {
            ram_.write32((address + index * 4) & kRamMask,
                         hardware_.mdec().read_word());
        }
        complete_mdec_dma(1);
        events_.record("dma1");
    }

    // See the interpreter: the decoder drives channel 1 through its request
    // line once the channel is armed, without the guest starting a transfer.
    void pump_mdec_dma() {
        const auto chcr = hardware_.dma_control(1);
        if ((chcr & 0x100u) == 0) return;
        if ((chcr & 1u) != 0) return;
        const auto pending = hardware_.mdec().pending_words();
        if (pending == 0) return;
        const auto counter = hardware_.dma_counter(1);
        const auto blocks = counter >> 16;
        auto words = counter & 0xFFFFu;
        if (words == 0) words = 0x10000u;
        if (blocks > 1) words *= blocks;
        const auto move =
            words < pending ? words : static_cast<std::uint32_t>(pending);
        run_dma1_words(move);
    }

    // DMA3 request-mode transfer from the CD data FIFO into main RAM.
    void run_dma3_transfer() {
        constexpr std::uint32_t kDefaultWords = 512;
        constexpr std::uint32_t kMaxWords = 1024;

        const auto counter = hardware_.dma_counter(3);
        const auto blocks = counter >> 16;
        auto words = counter & 0xFFFFu;
        if (words == 0) words = kDefaultWords;
        if (blocks > 1) words *= blocks;
        if (words > kMaxWords) words = kMaxWords;
        const auto address = 0x80000000u | hardware_.dma_address(3);
        const bool trace_dma3 = std::getenv("R6_TRACE_DMA3") != nullptr;
        if (trace_dma3) {
            // The cycle stamp is what lets a transfer be matched against the
            // CD events in the same log; without it a stopped DMA and a DMA
            // that never happened look identical.
            std::fprintf(stderr, "D3 %llu addr=%08X words=%u counter=%08X\n",
                         static_cast<unsigned long long>(hardware_.cycles()), address, words,
                         counter);
        }
        for (std::uint32_t index = 0; index < words; ++index) {
            const auto value = hardware_.cdrom().read_word();
            const auto target = (address + index * 4) & kRamMask;
            if (trace_dma3 && index < 4) {
                std::fprintf(stderr, "W3 %08X <- %08X\n", target, value);
            }
            ram_.write32(target, value);
        }
        hardware_.refresh_cdrom_interrupt();
        // Match the interpreter: raise the channel-3 completion when DICR has
        // the master and channel-3 enables that DMACallback installs, or when
        // the older cinema streaming slot is set.
        const auto dicr = hardware_.read32(hardware::kDmaInterrupt);
        const bool completion_requested = words > 8;
        const bool interrupt =
            ram_.read32(0x800B4394u & kRamMask) != 0 ||
            (completion_requested && (dicr & 0x00880000u) == 0x00880000u);
        hardware_.schedule_dma_completion(3, 16u, interrupt);
        events_.record("dma3");
    }

    // DMA4 feeds sound RAM, where the voice model reads its ADPCM headers.
    void run_dma4_transfer() {
        const auto counter = hardware_.dma_counter(4);
        auto words = counter & 0xFFFFu;
        if (words == 0) words = 0x10000u;
        const auto address = 0x80000000u | hardware_.dma_address(4);
        for (std::uint32_t index = 0; index < words; ++index) {
            hardware_.write_spu_ram_word(
                ram_.read32((address + index * 4) & kRamMask));
        }
        hardware_.complete_dma(4);
        events_.record("dma4");
    }

    static void note_watch(std::uint32_t address, const char* kind) {
        if (watch_address_ == 0) return;
        const auto physical = address & 0x1FFFFFFFu;
        const auto watched = watch_address_ & 0x1FFFFFFFu;
        if (physical != watched) return;
        ++watch_hits_;
        if (watch_hits_ <= 16) {
            std::fprintf(stderr, "watch %s 0x%08X (hit %llu)\n", kind, address,
                         static_cast<unsigned long long>(watch_hits_));
        }
    }

    inline static std::uint32_t watch_address_ = 0;
    inline static std::uint64_t watch_hits_ = 0;

    static constexpr std::uint32_t kRamMask = 0x1FFFFFu;
    static constexpr std::uint32_t kScratchpadBase = 0x1F800000u;
    static constexpr std::uint32_t kScratchpadMask = 0x3FFu;
    static constexpr std::uint32_t kHardwareBase = 0x1F801000u;
    static constexpr std::uint32_t kHardwareEnd = 0x1F803000u;
    static constexpr std::uint64_t kInterruptStepBudget = 2000000u;
    static constexpr std::uint64_t kMaxInterruptOverruns = 1000u;

    Memory& ram_;
    PlatformServices& services_;
    HardwareMap hardware_;
    Gte gte_;
    std::array<std::uint8_t, 0x400> scratchpad_{};
    EventTrace events_;
    bool vblank_enabled_{false};
    bool block_boundary_interrupts_{false};
    std::uint64_t vblank_next_{0};
    std::uint64_t vblank_polls_{0};
    std::uint64_t vblank_count_{0};
    bool interrupts_enabled_{true};
    bool in_interrupt_{false};
    std::uint32_t interrupt_saved_sr_{0};
    std::array<std::uint32_t, 32> interrupt_gpr_{};
    std::uint32_t interrupt_lo_{0};
    std::uint32_t interrupt_hi_{0};
    bool interrupt_context_valid_{false};
    std::uint64_t interrupt_steps_{0};
    std::uint64_t interrupt_overruns_{0};
    std::uint64_t interrupts_delivered_{0};
    std::uint64_t missing_vectors_{0};
};

using NativeBlock = std::uint32_t (*)(CpuState&, NativeBus&);

// Dispatches generated blocks that share one persistent NativeBus. The bus
// owns the device, GTE and scratchpad state for the whole run, so a block
// boundary no longer resets hardware state.
class NativeBlockDispatcher {
public:
    void register_function(std::uint32_t address, NativeBlock function) {
        functions_[address] = function;
    }

    bool contains(std::uint32_t address) const {
        return functions_.find(address) != functions_.end();
    }

    std::uint32_t step(CpuState& cpu, NativeBus& bus) const {
        const auto found = functions_.find(cpu.pc);
        if (found == functions_.end()) {
            throw std::out_of_range("no translated block registered for PC");
        }
        return found->second(cpu, bus);
    }

private:
    std::unordered_map<std::uint32_t, NativeBlock> functions_;
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
