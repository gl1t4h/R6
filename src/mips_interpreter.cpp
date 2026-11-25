#include "r6/mips_interpreter.h"

#include <cstdio>

#include <cstddef>
#include <cstdint>

namespace r6 {

namespace {

constexpr std::uint32_t kRamMask = 0x001FFFFFu;      // 2 MiB main RAM
constexpr std::uint32_t kScratchpadBase = 0x1F800000u;
constexpr std::uint32_t kScratchpadMask = 0x3FFu;
constexpr std::uint32_t kHardwareBase = 0x1F801000u;
constexpr std::uint32_t kHardwareEnd = 0x1F803000u;
constexpr std::uint32_t kDmaBase = 0x1F801080u;
constexpr std::uint32_t kDmaEnd = 0x1F801100u;
// Sentinel return address used to call a guest interrupt routine from the run
// loop and notice when it returns.
constexpr std::uint32_t kInterruptReturnAddress = 0x0000FFFCu;
constexpr std::uint64_t kInterruptStepBudget = 50000u;
constexpr std::uint64_t kMaxInterruptOverruns = 1000;

constexpr std::uint32_t reg(std::uint32_t instruction, std::uint32_t shift) {
    return (instruction >> shift) & 0x1Fu;
}

constexpr std::uint32_t immediate(std::uint32_t instruction) {
    return instruction & 0xFFFFu;
}

constexpr std::uint32_t sign_extend16(std::uint32_t value) {
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFFu)));
}

}  // namespace

MipsInterpreter::MipsInterpreter(Memory& ram, PlatformServices& services)
    : ram_(ram),
      services_(services),
      hardware_(services),
      scratchpad_(0x400, 0),
      cop0_(32, 0),
      gte_data_(32, 0) {
    // The retail kernel hands an application a status register with the global
    // interrupt enable and the hardware (IP2) mask already set; guest code
    // read-modify-writes SR to enable COP2 and relies on those bits surviving.
    cop0_[12] = 0x00000401u;
    // The disc controller reports completion through I_STAT bit 2, which is
    // what libcd's command state machine waits for.
    hardware_.set_cdrom_interrupt_enabled(true);
}

std::uint32_t MipsInterpreter::read8(std::uint32_t address) {
    const auto physical = address & 0x1FFFFFFFu;
    if (hardware_.is_cdrom(physical)) return hardware_.read_cdrom(physical);
    if (physical <= kRamMask) return ram_.read8(physical & kRamMask);
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        return scratchpad_[physical & kScratchpadMask];
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        return read32(physical & ~3u) >> ((physical & 3u) * 8u);
    }
    ++unknown_hardware_reads_;
    return 0;
}

std::uint32_t MipsInterpreter::read16(std::uint32_t address) {
    const auto physical = address & 0x1FFFFFFFu;
    if (hardware_.is_cdrom(physical)) {
        return hardware_.read_cdrom(physical) |
               (static_cast<std::uint32_t>(hardware_.read_cdrom(physical + 1))
                << 8);
    }
    if (physical <= kRamMask) return ram_.read16(physical & kRamMask);
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~1u;
        return static_cast<std::uint32_t>(scratchpad_[index]) |
               (static_cast<std::uint32_t>(scratchpad_[index + 1]) << 8);
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        return read32(physical & ~3u) >> ((physical & 2u) * 8u);
    }
    ++unknown_hardware_reads_;
    return 0;
}

std::uint32_t MipsInterpreter::read32(std::uint32_t address) {
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
    if (physical <= kRamMask) return ram_.read32(physical & kRamMask);
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~3u;
        return static_cast<std::uint32_t>(scratchpad_[index]) |
               (static_cast<std::uint32_t>(scratchpad_[index + 1]) << 8) |
               (static_cast<std::uint32_t>(scratchpad_[index + 2]) << 16) |
               (static_cast<std::uint32_t>(scratchpad_[index + 3]) << 24);
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        ++hardware_reads_[physical & ~3u];
        return hardware_.read32(physical);
    }
    ++unknown_hardware_reads_;
    return 0;
}

void MipsInterpreter::write8(std::uint32_t address, std::uint32_t value) {
    const auto physical = address & 0x1FFFFFFFu;
    if (hardware_.is_cdrom(physical)) {
        hardware_.write_cdrom(physical, static_cast<std::uint8_t>(value));
        return;
    }
    if (physical <= kRamMask) {
        ram_.write8(physical & kRamMask, static_cast<std::uint8_t>(value));
        return;
    }
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        scratchpad_[physical & kScratchpadMask] =
            static_cast<std::uint8_t>(value);
        return;
    }
    ++unknown_hardware_writes_;
}

void MipsInterpreter::write16(std::uint32_t address, std::uint32_t value) {
    const auto physical = address & 0x1FFFFFFFu;
    if (hardware_.is_cdrom(physical)) {
        hardware_.write_cdrom(physical, static_cast<std::uint8_t>(value));
        if (hardware_.is_cdrom(physical + 1)) {
            hardware_.write_cdrom(physical + 1,
                                  static_cast<std::uint8_t>(value >> 8));
        }
        return;
    }
    if (physical <= kRamMask) {
        ram_.write16(physical & kRamMask, static_cast<std::uint16_t>(value));
        return;
    }
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~1u;
        scratchpad_[index] = static_cast<std::uint8_t>(value);
        scratchpad_[index + 1] = static_cast<std::uint8_t>(value >> 8);
        return;
    }
    // Hardware registers are 32 bits wide, and guest code stores to them a
    // half at a time (the interrupt dispatcher acknowledges I_STAT that way).
    const auto aligned = physical & ~3u;
    const auto shift = (physical & 3u) * 8u;
    if (aligned == hardware::kInterruptStatus ||
        aligned == hardware::kInterruptMask) {
        // I_STAT clears every bit written as one, so the untouched half has to
        // be zero rather than read back and written again.
        write32(aligned, (value & 0xFFFFu) << shift);
        return;
    }
    const auto current = hardware_.read32(aligned);
    const auto mask = 0xFFFFu << shift;
    write32(aligned, (current & ~mask) | ((value & 0xFFFFu) << shift));
}

void MipsInterpreter::write32(std::uint32_t address, std::uint32_t value) {
    const auto physical = address & 0x1FFFFFFFu;
    if (trace_log_.size() < 24) {
        if (trace_reg_ != 0 && (physical & 0x1FFFFFFCu) == (trace_reg_ & 0x1FFFFFFCu)) {
            trace_log_.push_back("reg 0x" + format_hex(physical) + " = 0x" +
                                 format_hex(value) + " pc=0x" + format_hex(cpu_.pc));
        } else if (find_value_ != 0 && value == find_value_ &&
                   physical <= kRamMask) {
            trace_log_.push_back("store 0x" + format_hex(value) + " at 0x" +
                                 format_hex(physical) + " pc=0x" +
                                 format_hex(cpu_.pc));
        }
    }
    if (hardware_.is_cdrom(physical)) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            if (hardware_.is_cdrom(physical + index)) {
                hardware_.write_cdrom(physical + index,
                                      static_cast<std::uint8_t>(value >> (index * 8)));
            }
        }
        return;
    }
    if (physical <= kRamMask) {
        ram_.write32(physical & kRamMask, value);
        return;
    }
    if (physical >= kScratchpadBase && physical <= kScratchpadBase + kScratchpadMask) {
        const auto index = physical & kScratchpadMask & ~3u;
        scratchpad_[index] = static_cast<std::uint8_t>(value);
        scratchpad_[index + 1] = static_cast<std::uint8_t>(value >> 8);
        scratchpad_[index + 2] = static_cast<std::uint8_t>(value >> 16);
        scratchpad_[index + 3] = static_cast<std::uint8_t>(value >> 24);
        return;
    }
    if (physical >= kHardwareBase && physical < kHardwareEnd) {
        ++hardware_writes_[physical & ~3u];
        if (physical >= kDmaBase && physical < kDmaEnd) {
            ++dma_writes_;
        }
        hardware_.write32(physical, value);
        if (physical == hardware::kDma2ChannelControl) {
            const auto start = (value & (1u << 24)) != 0;
            const auto sync_mode = (value >> 9) & 3u;
            // Channel control bit 0 is set when the transfer reads main RAM.
            const auto from_ram = (value & 1u) != 0;
            if (start && sync_mode == 2 && from_ram) {
                run_dma2_list();
            }
        }
        if (physical == hardware::kDma3ChannelControl) {
            const auto start = (value & (1u << 24)) != 0;
            // Channel 3 reads the disc, so it moves data to main RAM.
            const auto to_ram = (value & 1u) == 0;
            if (start && to_ram) {
                run_dma3_transfer();
            }
        }
        return;
    }
    ++unknown_hardware_writes_;
}

bool MipsInterpreter::service_bios_call() {
    return services_.bios_call(cpu_.pc, cpu_, ram_);
}

void MipsInterpreter::record_call_site(std::uint32_t pc) {
    constexpr std::size_t kMaxCallSites = 16;
    if (recent_calls_.size() >= kMaxCallSites) {
        recent_calls_.erase(recent_calls_.begin());
    }
    recent_calls_.push_back(pc);
}

void MipsInterpreter::deliver_interrupt() {
    if (!interrupts_enabled_) return;
    // The R3000A only takes an external interrupt when COP0 SR has the global
    // enable and the hardware (IP2) mask bit set. Guest code sits in masked
    // critical sections constantly during init, and entering a handler there
    // resumes into half-updated state.
    const auto status = cop0_[12];
    if ((status & 0x1u) == 0 || (status & 0x400u) == 0) return;
    const auto pending = hardware_.interrupt_status() & hardware_.interrupt_mask();
    if (pending == 0) return;

    interrupt_routines_.clear();
    // The priority chains are the handlers; B(19h) HookEntryInt adds an entry
    // point the kernel runs once those chains have finished.
    for (std::uint32_t priority = 0; priority < 3; ++priority) {
        // SysEnqIntRP keeps a linked list per priority: each node is
        // { next, void (*routine)() }, and every node is serviced.
        auto entry = services_.interrupt_entry(priority);
        for (std::size_t depth = 0; entry != 0 && depth < 16; ++depth) {
            const auto routine = read32(entry + 4);
            if (routine >= 0x80000000u && routine < 0x80200000u) {
                interrupt_routines_.push_back(routine);
            }
            entry = read32(entry);
        }
    }
    const auto has_entry_int = services_.entry_int() != 0;
    if (interrupt_routines_.empty() && !has_entry_int) return;
    {
        interrupt_return_pc_ = cpu_.pc;
        interrupt_saved_ra_ = cpu_.gpr[31];
        for (std::size_t index = 0; index < 32; ++index) {
            interrupt_saved_gpr_[index] = cpu_.gpr[index];
        }
        interrupt_saved_hi_ = cpu_.hi;
        interrupt_saved_lo_ = cpu_.lo;
        interrupt_saved_cause_ = cop0_[13];
        interrupt_pending_bits_ = pending;
        if (interrupt_log_.size() < 6) {
            std::string line = "routines=";
            for (auto address : interrupt_routines_) {
                line += "0x" + format_hex(address) + " ";
            }
            line += "sp=0x" + format_hex(cpu_.gpr[29]);
            line += " gp=0x" + format_hex(cpu_.gpr[28]);
            line += " interrupted=0x" + format_hex(cpu_.pc);
            interrupt_log_.push_back(line);
        }
        interrupt_steps_ = 0;
        irq_pcs_.clear();
        irq_hot_.clear();
        if (irq_traces_.size() < 4) {
            irq_traces_.emplace_back();
            irq_traces_.back().reserve(32);
        }
        // The retail kernel enters the dispatcher with the hardware interrupt
        // pending bit set in CAUSE and defined argument registers. Leaving the
        // interrupted values in $a0-$a3 makes handlers act on stale pointers.
        cop0_[13] |= 0x00000400u;  // CAUSE.IP2
        for (std::size_t reg = 4; reg < 8; ++reg) cpu_.gpr[reg] = 0;
        cpu_.gpr[31] = kInterruptReturnAddress;
        // An exception entry returns through $k0 as often as through $ra.
        cpu_.gpr[26] = kInterruptReturnAddress;
        interrupt_routine_index_ = 0;
        interrupt_hook_done_ = !has_entry_int;
        in_interrupt_ = true;
        ++interrupts_delivered_;
        interrupt_steps_ = 0;
        irq_pcs_.clear();
        irq_hot_.clear();
        if (has_entry_int) {
            // libetc's dispatcher runs before the priority chains: the guest's
            // own handlers acknowledge I_STAT eagerly, and the dispatcher can
            // only route an interrupt whose bit is still pending.
            enter_entry_int();
            return;
        }
        cpu_.pc = interrupt_routines_[0];
        interrupt_routine_index_ = 1;
        return;
    }
}

void MipsInterpreter::enter_entry_int() {
    // B(19h) HookEntryInt structure (setjmp layout): the kernel calls the
    // saved pc with $v0 = 1 and $a0 pointing at the structure.
    const auto entry_int = services_.entry_int();
    cpu_.pc = read32(entry_int);
    cpu_.gpr[2] = 1;
    cpu_.gpr[4] = entry_int;
    cpu_.gpr[29] = read32(entry_int + 4);
    cpu_.gpr[30] = read32(entry_int + 8);
    for (std::uint32_t index = 0; index < 8; ++index) {
        cpu_.gpr[16 + index] = read32(entry_int + 12 + index * 4);
    }
    cpu_.gpr[28] = read32(entry_int + 0x2C);
    cpu_.gpr[31] = kInterruptReturnAddress;
}

void MipsInterpreter::poll_vblank() {
    constexpr std::uint32_t kVblankBit = 1u;
    constexpr std::uint32_t kVblankPeriodMicros = 16667u;

    const auto now = services_.read_timer(0);
    if (vblank_next_ == 0) {
        vblank_next_ = now + kVblankPeriodMicros;
        return;
    }
    if (now < vblank_next_) return;
    vblank_next_ = now + kVblankPeriodMicros;
    ++vblank_count_;
    hardware_.raise_interrupt(kVblankBit);
}

void MipsInterpreter::run_dma3_transfer() {
    constexpr std::uint32_t kDefaultWords = 512;
    constexpr std::uint32_t kMaxWords = 1024;

    auto words = hardware_.dma_counter(3) & 0xFFFFu;
    if (words == 0 || words > kMaxWords) words = kDefaultWords;
    const auto address = hardware_.dma_address(3);
    for (std::uint32_t index = 0; index < words; ++index) {
        write32(address + index * 4, hardware_.cdrom().read_word());
    }
    hardware_.refresh_cdrom_interrupt();
}

void MipsInterpreter::run_dma2_list() {
    constexpr std::uint32_t kEndMarker = 0x00FFFFFFu;
    constexpr std::uint32_t kRamAddressMask = 0x001FFFFCu;
    constexpr std::size_t kMaxNodes = 4096;
    constexpr std::uint64_t kMaxWords = 1u << 20;

    auto address = hardware_.dma_address(2) & kRamAddressMask;
    ++dma_lists_;

    for (std::size_t node = 0; node < kMaxNodes && dma_words_ < kMaxWords;
         ++node) {
        // Packet tag: bits 24-31 hold the payload word count and bits 0-23 the
        // address of the next tag.
        const auto tag = read32(0x80000000u | address);
        const auto words = tag >> 24;
        const auto next = tag & kEndMarker;
        for (std::uint32_t word = 0; word < words; ++word) {
            const auto source =
                (address + 4 + word * 4) & kRamAddressMask;
            services_.write_gpu_register(hardware::kGpuCommand,
                                         read32(0x80000000u | source));
            ++dma_words_;
        }
        if (next == kEndMarker) break;
        address = next & kRamAddressMask;
    }
}

std::string MipsInterpreter::format_hex(std::uint32_t value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%08X", value);
    return buffer;
}

RunResult MipsInterpreter::run(std::uint64_t max_steps) {
    RunResult result;
    bool in_delay_slot = false;
    std::uint32_t delay_target = 0;

    while (result.steps < max_steps) {
        const auto current_pc = cpu_.pc;
        for (auto& entry : watched_) {
            if (entry.first == current_pc) ++entry.second;
        }
        if (hot_enabled_ && (result.steps & 0x3Fu) == 0) ++hot_[current_pc];
        const bool was_delay_slot = in_delay_slot;
        const auto target = delay_target;
        in_delay_slot = false;

        if (current_pc == kInterruptReturnAddress) {
            if (!interrupt_hook_done_ && services_.entry_int() != 0) {
                interrupt_hook_done_ = true;
                ++interrupt_hooks_;
                enter_entry_int();
                interrupt_steps_ = 0;
                irq_pcs_.clear();
                irq_hot_.clear();
                continue;
            }
            if (interrupt_routine_index_ < interrupt_routines_.size()) {
                // More queued routines belong to this interrupt: enter the next
                // one from the interrupted context, as the kernel does.
                const auto routine =
                    interrupt_routines_[interrupt_routine_index_++];
                for (std::size_t index = 0; index < 32; ++index) {
                    cpu_.gpr[index] = interrupt_saved_gpr_[index];
                }
                cpu_.hi = interrupt_saved_hi_;
                cpu_.lo = interrupt_saved_lo_;
                cpu_.gpr[31] = kInterruptReturnAddress;
                cpu_.pc = routine;
                interrupt_steps_ = 0;
                irq_pcs_.clear();
                irq_hot_.clear();
                continue;
            }
            // Every handler returned; resume the interrupted instruction
            // stream exactly where it left off, with its registers.
            cpu_.pc = interrupt_return_pc_;
            for (std::size_t index = 0; index < 32; ++index) {
                cpu_.gpr[index] = interrupt_saved_gpr_[index];
            }
            cpu_.hi = interrupt_saved_hi_;
            cpu_.lo = interrupt_saved_lo_;
            cop0_[13] = interrupt_saved_cause_;
            hardware_.acknowledge_interrupts(interrupt_pending_bits_);
            in_interrupt_ = false;
            continue;
        }

        if (in_interrupt_) {
            ++irq_hot_[current_pc];
            if (++interrupt_steps_ > kInterruptStepBudget) {
                // The routine left its own context, so drop the call and resume
                // the interrupted stream. After a few of these, stop delivering
                // interrupts rather than burning the budget on them.
                cpu_.pc = interrupt_return_pc_;
                for (std::size_t index = 0; index < 32; ++index) {
                cpu_.gpr[index] = interrupt_saved_gpr_[index];
            }
                cpu_.hi = interrupt_saved_hi_;
                cpu_.lo = interrupt_saved_lo_;
                cop0_[13] = interrupt_saved_cause_;
                hardware_.acknowledge_interrupts(interrupt_pending_bits_);
                in_interrupt_ = false;
                if (++interrupt_overruns_ >= kMaxInterruptOverruns) {
                    interrupts_enabled_ = false;
                }
                continue;
            }
        } else if (!was_delay_slot && (result.steps & 0x3FFu) == 0) {
            // A vblank only starts from the main flow: generating one while a
            // handler runs makes the dispatcher re-enter itself for every poll.
            if (!in_interrupt_ && vblank_enabled_) poll_vblank();
            hardware_.pad_tick();
        }
        if (!in_interrupt_ && !was_delay_slot && (result.steps & 0x3FFu) == 0) {
            // Never interrupt a delay slot: the pending branch target lives in
            // the run loop, not in guest state, so resuming mid-pair would
            // lose it.
            deliver_interrupt();
            // The handler jump replaced cpu_.pc, so current_pc is stale: restart
            // the iteration and execute the handler instead of the instruction
            // that was interrupted.
            if (in_interrupt_) continue;
        }

        if (current_pc == 0) {
            result.reason = StopReason::returned;
            result.pc = current_pc;
            return result;
        }

        if (service_bios_call()) {
            ++result.steps;
            cpu_.pc = services_.take_return_from_exception() && in_interrupt_
                          ? kInterruptReturnAddress
                          : cpu_.gpr[31];
            if (recent_pcs_.size() >= 16) recent_pcs_.erase(recent_pcs_.begin());
            recent_pcs_.push_back(current_pc);
            continue;
        }

        const auto instruction = read32(current_pc);
        const auto step = execute(instruction, current_pc);
        ++result.steps;

        if (recent_pcs_.size() >= 16) recent_pcs_.erase(recent_pcs_.begin());
        recent_pcs_.push_back(current_pc);
        if (in_interrupt_ && irq_pcs_.size() < 64) irq_pcs_.push_back(current_pc);
        if (in_interrupt_ && !irq_traces_.empty() &&
            irq_traces_.back().size() < 32) {
            irq_traces_.back().push_back(current_pc);
        }

        if (step.unsupported) {
            result.reason = StopReason::unsupported_instruction;
            result.pc = current_pc;
            result.instruction = instruction;
            return result;
        }

        if (step.halted) {
            result.reason = StopReason::guest_break;
            result.pc = current_pc;
            result.instruction = instruction;
            return result;
        }

        cpu_.gpr[0] = 0;
        if (step.branch) {
            in_delay_slot = true;
            delay_target = step.target;
        }

        if (was_delay_slot) {
            cpu_.pc = target;
        } else if (step.branch) {
            cpu_.pc = current_pc + 4;
        } else {
            cpu_.pc = current_pc + 4;
        }
    }

    result.reason = StopReason::step_budget;
    result.pc = cpu_.pc;
    return result;
}

MipsInterpreter::StepResult MipsInterpreter::execute(
    std::uint32_t instruction,
    std::uint32_t pc) {
    StepResult result;
    auto& gpr = cpu_.gpr;
    const auto opcode = instruction >> 26;
    const auto rs = reg(instruction, 21);
    const auto rt = reg(instruction, 16);
    const auto rd = reg(instruction, 11);
    const auto shamt = reg(instruction, 6);

    switch (opcode) {
        case 0x00: {  // SPECIAL
            switch (instruction & 0x3Fu) {
                case 0x00:
                    gpr[rd] = gpr[rt] << shamt;
                    break;
                case 0x02:
                    gpr[rd] = gpr[rt] >> shamt;
                    break;
                case 0x03:
                    gpr[rd] = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(gpr[rt]) >> shamt);
                    break;
                case 0x04:
                    gpr[rd] = gpr[rt] << (gpr[rs] & 0x1Fu);
                    break;
                case 0x06:
                    gpr[rd] = gpr[rt] >> (gpr[rs] & 0x1Fu);
                    break;
                case 0x07:
                    gpr[rd] = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(gpr[rt]) >>
                        static_cast<int>(gpr[rs] & 0x1Fu));
                    break;
                case 0x08:
                    result.branch = true;
                    result.target = gpr[rs];
                    break;
                case 0x09: {
                    const auto link = pc + 8;
                    record_call_site(pc);
                    result.branch = true;
                    result.target = gpr[rs];
                    gpr[rd] = link;
                    break;
                }
                case 0x0C:  // syscall
                    break;
                case 0x0D:  // break
                    result.halted = true;
                    break;
                case 0x10:
                    gpr[rd] = cpu_.hi;
                    break;
                case 0x11:
                    cpu_.hi = gpr[rs];
                    break;
                case 0x12:
                    gpr[rd] = cpu_.lo;
                    break;
                case 0x13:
                    cpu_.lo = gpr[rs];
                    break;
                case 0x18: {
                    const auto product =
                        static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[rs])) *
                        static_cast<std::int64_t>(static_cast<std::int32_t>(gpr[rt]));
                    cpu_.lo = static_cast<std::uint32_t>(product);
                    cpu_.hi = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(product) >> 32);
                    break;
                }
                case 0x19: {
                    const auto product =
                        static_cast<std::uint64_t>(gpr[rs]) *
                        static_cast<std::uint64_t>(gpr[rt]);
                    cpu_.lo = static_cast<std::uint32_t>(product);
                    cpu_.hi = static_cast<std::uint32_t>(product >> 32);
                    break;
                }
                case 0x1A: {
                    const auto numerator = static_cast<std::int32_t>(gpr[rs]);
                    const auto denominator = static_cast<std::int32_t>(gpr[rt]);
                    if (denominator != 0) {
                        cpu_.lo = static_cast<std::uint32_t>(
                            numerator / denominator);
                        cpu_.hi = static_cast<std::uint32_t>(
                            numerator % denominator);
                    }
                    break;
                }
                case 0x1B: {
                    if (gpr[rt] != 0) {
                        cpu_.lo = gpr[rs] / gpr[rt];
                        cpu_.hi = gpr[rs] % gpr[rt];
                    }
                    break;
                }
                case 0x20:
                case 0x21:
                    gpr[rd] = gpr[rs] + gpr[rt];
                    break;
                case 0x22:
                case 0x23:
                    gpr[rd] = gpr[rs] - gpr[rt];
                    break;
                case 0x24:
                    gpr[rd] = gpr[rs] & gpr[rt];
                    break;
                case 0x25:
                    gpr[rd] = gpr[rs] | gpr[rt];
                    break;
                case 0x26:
                    gpr[rd] = gpr[rs] ^ gpr[rt];
                    break;
                case 0x27:
                    gpr[rd] = ~(gpr[rs] | gpr[rt]);
                    break;
                case 0x2A:
                    gpr[rd] = static_cast<std::int32_t>(gpr[rs]) <
                                      static_cast<std::int32_t>(gpr[rt])
                                  ? 1u
                                  : 0u;
                    break;
                case 0x2B:
                    gpr[rd] = gpr[rs] < gpr[rt] ? 1u : 0u;
                    break;
                default:
                    result.unsupported = true;
                    break;
            }
            break;
        }
        case 0x01: {  // REGIMM
            const auto offset = sign_extend16(immediate(instruction)) << 2;
            const auto target = pc + 4 + offset;
            switch (rt) {
                case 0x00:  // bltz
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) < 0)) {
                        result.target = pc + 8;
                    }
                    break;
                case 0x01:  // bgez
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) >= 0)) {
                        result.target = pc + 8;
                    }
                    break;
                case 0x10:  // bltzal
                    gpr[31] = pc + 8;
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) < 0)) {
                        result.target = pc + 8;
                    }
                    break;
                case 0x11:  // bgezal
                    gpr[31] = pc + 8;
                    result.branch = true;
                    result.target = target;
                    if (!(static_cast<std::int32_t>(gpr[rs]) >= 0)) {
                        result.target = pc + 8;
                    }
                    break;
                default:
                    result.unsupported = true;
                    break;
            }
            break;
        }
        case 0x02:  // j
            result.branch = true;
            result.target = (pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
            break;
        case 0x03:  // jal
            record_call_site(pc);
            gpr[31] = pc + 8;
            result.branch = true;
            result.target = (pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
            break;
        case 0x04:  // beq
            result.branch = true;
            result.target = gpr[rs] == gpr[rt]
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x05:  // bne
            result.branch = true;
            result.target = gpr[rs] != gpr[rt]
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x06:  // blez
            result.branch = true;
            result.target = static_cast<std::int32_t>(gpr[rs]) <= 0
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x07:  // bgtz
            result.branch = true;
            result.target = static_cast<std::int32_t>(gpr[rs]) > 0
                                ? pc + 4 + (sign_extend16(immediate(instruction)) << 2)
                                : pc + 8;
            break;
        case 0x08:  // addi
        case 0x09:  // addiu
            gpr[rt] = gpr[rs] + sign_extend16(immediate(instruction));
            break;
        case 0x0A:  // slti
            gpr[rt] = static_cast<std::int32_t>(gpr[rs]) <
                              static_cast<std::int32_t>(sign_extend16(immediate(instruction)))
                          ? 1u
                          : 0u;
            break;
        case 0x0B:  // sltiu
            gpr[rt] = gpr[rs] < sign_extend16(immediate(instruction)) ? 1u : 0u;
            break;
        case 0x0C:  // andi
            gpr[rt] = gpr[rs] & immediate(instruction);
            break;
        case 0x0D:  // ori
            gpr[rt] = gpr[rs] | immediate(instruction);
            break;
        case 0x0E:  // xori
            gpr[rt] = gpr[rs] ^ immediate(instruction);
            break;
        case 0x0F:  // lui
            gpr[rt] = immediate(instruction) << 16;
            break;
        case 0x10:  // COP0
            if (rs == 0x00) {
                gpr[rt] = cop0_[rd];
            } else if (rs == 0x04) {
                if (rd == 12 && sr_writes_.size() < 24) {
                    sr_writes_.emplace_back(pc, gpr[rt]);
                }
                cop0_[rd] = gpr[rt];
            } else if ((instruction & 0x3Fu) == 0x10) {  // rfe
                const auto status = cop0_[12];
                cop0_[12] = (status & ~0x3Fu) | ((status >> 2) & 0xFu);
            } else {
                result.unsupported = true;
            }
            break;
        case 0x12: {  // COP2 / GTE
            if (rs == 0x00) {
                gpr[rt] = gte_data_[rd];
            } else if (rs == 0x02) {
                gpr[rt] = gte_data_[rd];
            } else if (rs == 0x04) {
                gte_data_[rd] = gpr[rt];
            } else if (rs == 0x06) {
                gte_data_[rd] = gpr[rt];
            } else {
                // Geometry maths is not implemented yet; the command is
                // accepted so execution continues.
                ++gte_commands_;
            }
            break;
        }
        case 0x20:  // lb
            gpr[rt] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(
                    static_cast<std::int8_t>(read8(gpr[rs] + sign_extend16(immediate(instruction))))));
            break;
        case 0x21:  // lh
            gpr[rt] = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(
                    static_cast<std::int16_t>(read16(gpr[rs] + sign_extend16(immediate(instruction))))));
            break;
        case 0x22: {  // lwl
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            const auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    gpr[rt] = (gpr[rt] & 0x00FFFFFFu) | (aligned << 24);
                    break;
                case 1:
                    gpr[rt] = (gpr[rt] & 0x0000FFFFu) | (aligned << 16);
                    break;
                case 2:
                    gpr[rt] = (gpr[rt] & 0x000000FFu) | (aligned << 8);
                    break;
                default:
                    gpr[rt] = aligned;
                    break;
            }
            break;
        }
        case 0x23:  // lw
            gpr[rt] = read32(gpr[rs] + sign_extend16(immediate(instruction)));
            break;
        case 0x24:  // lbu
            gpr[rt] = read8(gpr[rs] + sign_extend16(immediate(instruction))) & 0xFFu;
            break;
        case 0x25:  // lhu
            gpr[rt] = read16(gpr[rs] + sign_extend16(immediate(instruction))) & 0xFFFFu;
            break;
        case 0x26: {  // lwr
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            const auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    gpr[rt] = aligned;
                    break;
                case 1:
                    gpr[rt] = (gpr[rt] & 0xFF000000u) | (aligned >> 8);
                    break;
                case 2:
                    gpr[rt] = (gpr[rt] & 0xFFFF0000u) | (aligned >> 16);
                    break;
                default:
                    gpr[rt] = (gpr[rt] & 0xFFFFFF00u) | (aligned >> 24);
                    break;
            }
            break;
        }
        case 0x28:  // sb
            write8(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt] & 0xFFu);
            break;
        case 0x29:  // sh
            write16(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt] & 0xFFFFu);
            break;
        case 0x2A: {  // swl
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    aligned = (aligned & 0xFFFFFF00u) | (gpr[rt] >> 24);
                    break;
                case 1:
                    aligned = (aligned & 0xFFFF0000u) | (gpr[rt] >> 16);
                    break;
                case 2:
                    aligned = (aligned & 0xFF000000u) | (gpr[rt] >> 8);
                    break;
                default:
                    aligned = gpr[rt];
                    break;
            }
            write32(address & ~3u, aligned);
            break;
        }
        case 0x2B:  // sw
            write32(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt]);
            break;
        case 0x2E: {  // swr
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            auto aligned = read32(address & ~3u);
            switch (address & 3u) {
                case 0:
                    aligned = gpr[rt];
                    break;
                case 1:
                    aligned = (aligned & 0x000000FFu) | (gpr[rt] << 8);
                    break;
                case 2:
                    aligned = (aligned & 0x0000FFFFu) | (gpr[rt] << 16);
                    break;
                default:
                    aligned = (aligned & 0x00FFFFFFu) | (gpr[rt] << 24);
                    break;
            }
            write32(address & ~3u, aligned);
            break;
        }
        case 0x32:  // lwc2
            gte_data_[rt] = read32(gpr[rs] + sign_extend16(immediate(instruction)));
            break;
        case 0x3A:  // swc2
            write32(gpr[rs] + sign_extend16(immediate(instruction)), gte_data_[rt]);
            break;
        default:
            result.unsupported = true;
            break;
    }

    return result;
}

}  // namespace r6
