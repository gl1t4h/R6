#include "r6/mips_interpreter.h"

#include "r6/unaligned.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

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
      cop0_(32, 0) {
    // The retail kernel hands an application a status register with the global
    // interrupt enable and the hardware (IP2) mask already set; guest code
    // read-modify-writes SR to enable COP2 and relies on those bits surviving.
    cop0_[12] = 0x00000401u;
    // The disc controller reports completion through I_STAT bit 2, which is
    // what libcd's command state machine waits for.
    hardware_.set_cdrom_interrupt_enabled(true);
    // Bring-up knobs for the interrupt-ordering investigation: the step
    // interval between polls and the controller's sector-delivery time.
    if (const char* poll = std::getenv("R6_IRQ_POLL")) {
        const auto value = std::strtoul(poll, nullptr, 0);
        irq_poll_ = value != 0 ? value : 1;
    }
    if (const char* delay = std::getenv("R6_CD_SECTOR_DELAY")) {
        hardware_.cdrom().set_sector_delay(
            std::strtoull(delay, nullptr, 0));
    }
    // Debug aid: the resident sound tick zeroes its work counter at
    // 0x80033F00, so it can only return 0 and the boot's substate-1 wait can
    // never complete. With this set, its return-0 stub reports one finished
    // slot instead, which lets the rest of the boot run be measured.
    if (const char* force = std::getenv("R6_FORCE_SOUND_TICK")) {
        force_sound_tick_ = std::strtoul(force, nullptr, 0) != 0;
    }
}

void MipsInterpreter::note_cdrom_poll(std::uint32_t physical) {
    if (physical != hardware::kCdromBase) return;
    if (cdrom_polls_.size() >= 16) cdrom_polls_.erase(cdrom_polls_.begin());
    cdrom_polls_.push_back(cpu_.pc);
}

void MipsInterpreter::note_watched_write(std::uint32_t address,
                                         std::uint32_t value, int width) {
    if (!watch_ranges_.empty()) {
        const auto bytes = static_cast<std::uint32_t>(width / 8);
        for (auto& range : watch_ranges_) {
            if (address + bytes <= range.begin || address >= range.end) {
                continue;
            }
            ++range.stores;
            range.bytes += bytes;
            ++range_hot_[cpu_.pc];
            if (range_samples_.size() < 6) {
                range_samples_.push_back(
                    "write" + std::to_string(width) + " 0x" +
                    format_hex(address) + " = 0x" + format_hex(value) +
                    " pc=0x" + format_hex(cpu_.pc));
            }
            break;
        }
    }
    if (watch_writes_.empty()) return;
    // Match the whole containing word so a 32-bit store and the 16-bit half
    // stores that patch the same word are both captured.
    const auto word = address & 0x1FFFFFFCu;
    bool watched = false;
    for (auto entry : watch_writes_) {
        if ((entry & 0x1FFFFFFCu) == word) {
            watched = true;
            break;
        }
    }
    if (!watched) return;
    // Rolling log: keep the most recent stores, since the value under
    // investigation is usually written long after startup.
    if (write_log_.size() >= 4096) write_log_.erase(write_log_.begin());
    write_log_.push_back("write" + std::to_string(width) + " 0x" +
                         format_hex(address) + " = 0x" + format_hex(value) +
                         " pc=0x" + format_hex(cpu_.pc) + " s0=0x" +
                         format_hex(cpu_.gpr[16]) + " s1=0x" +
                         format_hex(cpu_.gpr[17]));
}

void MipsInterpreter::note_vector_write(std::uint32_t address,
                                        std::uint32_t value, int width) {
    // The vector and the words a short handler stub occupies. These writes are
    // rare, so the first sixteen are kept with the PC that made each one.
    const auto physical = address & 0x1FFFFFFFu;
    const auto base = kExceptionVector & 0x1FFFFFFFu;
    if (physical < base || physical >= base + 0x20u) return;
    if (vector_writes_.size() >= 16) return;
    vector_writes_.push_back(
        VectorWrite{0x80000000u | physical, value, cpu_.pc, width});
}

std::uint32_t MipsInterpreter::read8(std::uint32_t address) {
    const auto physical = address & 0x1FFFFFFFu;
    if (hardware_.is_cdrom(physical)) {
        note_cdrom_poll(physical);
        return hardware_.read_cdrom(physical);
    }
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
        note_cdrom_poll(physical);
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
        // The SPU register block is 16 bits wide, so a halfword read must not
        // be rounded down to a 32-bit register the way I_STAT/DMA are.
        if (physical >= hardware::kSpuBase && physical < hardware::kSpuEnd) {
            return read32(physical) & 0xFFFFu;
        }
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
    note_watched_write(address, value, 8);
    note_vector_write(address, value, 8);
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
    note_watched_write(address, value, 16);
    note_vector_write(address, value, 16);
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
    if (physical >= hardware::kSpuBase && physical < hardware::kSpuEnd) {
        // SPU registers really are 16 bits wide; merging into a 32-bit word
        // would land the write on the neighbouring register.
        write32(physical, value & 0xFFFFu);
        return;
    }
    const auto aligned = physical & ~3u;
    const auto shift = (physical & 3u) * 8u;
    if (aligned == hardware::kInterruptStatus ||
        aligned == hardware::kInterruptMask) {
        // I_STAT clears the bits written as zero, so the untouched half has to
        // be preserved rather than zeroed. I_MASK is a plain latch and needs
        // the same merge.
        const auto current = hardware_.read32(aligned);
        const auto mask = 0xFFFFu << shift;
        write32(aligned, (current & ~mask) | ((value & 0xFFFFu) << shift));
        return;
    }
    const auto current = hardware_.read32(aligned);
    const auto mask = 0xFFFFu << shift;
    write32(aligned, (current & ~mask) | ((value & 0xFFFFu) << shift));
}

void MipsInterpreter::write32(std::uint32_t address, std::uint32_t value) {
    note_watched_write(address, value, 32);
    note_vector_write(address, value, 32);
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
        if (physical >= hardware::kDma3BaseAddress &&
            physical <= hardware::kDma3ChannelControl &&
            dma3_log_.size() < 64) {
            // Channel 3 register traffic, so a response transfer that the model
            // does not execute can be seen instead of guessed at.
            dma3_log_.push_back("dma3 reg 0x" + format_hex(physical) +
                                " = 0x" + format_hex(value) + " pc=0x" +
                                format_hex(cpu_.pc));
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
        if (physical == hardware::kDma4ChannelControl) {
            const auto start = (value & (1u << 24)) != 0;
            // Channel 4 feeds the SPU; bit 0 set means main RAM to device.
            const auto to_device = (value & 1u) != 0;
            if (start && to_device) {
                run_dma4_transfer();
            }
        }
        return;
    }
    ++unknown_hardware_writes_;
}

bool MipsInterpreter::service_bios_call() {
    // Temporary diagnostic: R6_TRACE_BIOS=1 logs each BIOS entry with its
    // arguments and return value so it can be diffed against the native port.
    static const bool trace_bios = std::getenv("R6_TRACE_BIOS") != nullptr;
    const bool traced =
        trace_bios && cpu_.pc >= 0xA0u && cpu_.pc <= 0xC4u;
    const auto pc = cpu_.pc;
    const auto call_number = cpu_.gpr[9];
    const auto a0 = cpu_.gpr[4];
    const auto a1 = cpu_.gpr[5];
    const auto a2 = cpu_.gpr[6];
    const auto cycles = hardware_.cycles();
    const bool called = services_.bios_call(cpu_.pc, cpu_, ram_);
    if (traced) {
        std::printf("BC %llu 0x%08X t1=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X v0=0x%08X\n",
                    static_cast<unsigned long long>(cycles), pc, call_number, a0,
                    a1, a2, cpu_.gpr[2]);
    }
    return called;
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

    // Real hardware has the BIOS exception handler at the vector, so games
    // install no handler of their own there. When the vector is empty, run
    // the guest's HookEntryInt jmp_buf instead: the retail kernel does that
    // at the end of every exception, and libetc dispatches its interrupt
    // callbacks from it. Only when the guest hooked neither is there nowhere
    // to go, and delivered interrupts would slide through zeroed RAM.
    const auto vector = read32(kExceptionVector);
    const auto entry_int = services_.entry_int();
    const auto entry_pc = entry_int != 0 ? read32(entry_int) : 0;
    if (vector == 0 && entry_pc == 0) {
        ++missing_vectors_;
        char event[96];
        std::snprintf(event, sizeof(event),
                      "missing_vector pending=0x%08X pc=0x%08X", pending,
                      cpu_.pc);
        events_.record(event);
        if (interrupt_log_.size() < 6) {
            interrupt_log_.push_back(
                "no handler at 0x" + format_hex(kExceptionVector) +
                " bits=0x" + format_hex(pending) + " pc=0x" +
                format_hex(cpu_.pc));
        }
        return;
    }

    interrupt_saved_sr_ = status;
    // Hardware exception entry, ExcCode 0 (interrupt). EPC keeps the PC of the
    // instruction that did not run, CAUSE shows the summary line the kernel
    // reads, and SR pushes IEc/KUc into IEp/KUp and clears IEc. That last part
    // is what keeps a handler from being re-entered before its RFE.
    cop0_[14] = cpu_.pc;
    cop0_[13] = (cop0_[13] & ~0x0000FF7Cu) | 0x00000400u;
    cop0_[12] = (status & ~0x3Fu) | ((status & 0x0Fu) << 2);
    save_interrupt_context();
    if (vector != 0) {
        cpu_.pc = kExceptionVector;
    } else {
        // longjmp(EntryInt, 1): the jmp_buf that A(13h) setjmp filled keeps
        // RA, SP, FP, S0-S7 and GP, so restoring them is the C-level resume
        // libetc expects. EPC still holds the interrupted PC for the handler's
        // B(17h) ReturnFromException.
        cpu_.gpr[31] = read32(entry_int);
        cpu_.gpr[29] = read32(entry_int + 4);
        cpu_.gpr[30] = read32(entry_int + 8);
        for (std::uint32_t index = 0; index < 8; ++index) {
            cpu_.gpr[16 + index] = read32(entry_int + 12 + index * 4);
        }
        cpu_.gpr[28] = read32(entry_int + 0x2C);
        cpu_.gpr[2] = 1;  // longjmp returns 1 from the matching setjmp.
        cpu_.pc = entry_pc;
    }
    in_interrupt_ = true;
    interrupt_steps_ = 0;
    irq_pcs_.clear();
    irq_hot_.clear();
    irq_tail_size_ = 0;
    irq_tail_next_ = 0;
    if (irq_traces_.size() < 4) {
        irq_traces_.emplace_back();
        irq_traces_.back().reserve(32);
    }
    if (interrupt_log_.size() < 6) {
        interrupt_log_.push_back(
            std::string(vector != 0 ? "vector bits=0x" : "entryint bits=0x") +
            format_hex(pending) + " interrupted=0x" +
            format_hex(cop0_[14]));
    }
    ++interrupts_delivered_;
    char event[128];
    std::snprintf(event, sizeof(event),
                  "%s pending=0x%08X epc=0x%08X target=0x%08X",
                  vector != 0 ? "interrupt" : "entryint", pending,
                  cop0_[14], cpu_.pc);
    events_.record(event);
}

void MipsInterpreter::return_from_exception() {
    const auto status = cop0_[12];
    cop0_[12] = (status & ~0x3Fu) | ((status >> 2) & 0xFu);
}

void MipsInterpreter::save_interrupt_context() {
    for (std::size_t index = 0; index < 32; ++index) {
        interrupt_gpr_[index] = cpu_.gpr[index];
    }
    interrupt_lo_ = cpu_.lo;
    interrupt_hi_ = cpu_.hi;
    interrupt_context_valid_ = true;
}

void MipsInterpreter::restore_interrupt_context() {
    if (!interrupt_context_valid_) return;
    for (std::size_t index = 0; index < 32; ++index) {
        cpu_.gpr[index] = interrupt_gpr_[index];
    }
    cpu_.lo = interrupt_lo_;
    cpu_.hi = interrupt_hi_;
    interrupt_context_valid_ = false;
}

void MipsInterpreter::poll_vblank() {
    constexpr std::uint32_t kVblankBit = 1u;
    // The R3000A runs at 33.8688 MHz and a field lasts 1/60 s.
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

void MipsInterpreter::run_dma3_transfer() {
    constexpr std::uint32_t kDefaultWords = 512;
    constexpr std::uint32_t kMaxWords = 1024;

    // Channel 3 runs in request mode: the counter holds the number of blocks in
    // the high half and the block size in words in the low half.
    const auto counter = hardware_.dma_counter(3);
    const auto blocks = counter >> 16;
    auto words = counter & 0xFFFFu;
    if (words == 0) words = kDefaultWords;
    if (blocks > 1) words *= blocks;
    if (words > kMaxWords) words = kMaxWords;
    // The counter holds a physical address; the CPU only sees KSEG0, so map it
    // back before storing. Dropping the segment is why every transfer used to
    // land outside RAM and vanish.
    const auto address = 0x80000000u | hardware_.dma_address(3);
    if (dma3_log_.size() < 4096) {
        dma3_log_.push_back("run madr=0x" + format_hex(address) +
                            " bcr=0x" + format_hex(hardware_.dma_counter(3)) +
                            " words=" + std::to_string(words) + " cd=" +
                            std::to_string(hardware_.cdrom().sector()) +
                            " pc=0x" + format_hex(cpu_.pc));
    }
    for (std::uint32_t index = 0; index < words; ++index) {
        write32(address + index * 4, hardware_.cdrom().read_word());
    }
    hardware_.refresh_cdrom_interrupt();
    // A finished CD transfer is a DMA completion too: the guest's DMA handler
    // dispatches on the channel flag in DICR (0x1F8010F4) and, for channel 3,
    // calls whatever callback the current subsystem installed in the channel
    // table at 0x800B4388. libcd reads its own sectors by polling CHCR and
    // installs no callback, so an interrupt there would only preempt its
    // handshake; the cinema stream installs one (0x8008DB44) and needs the
    // completion so it can publish the buffered record. Raise the interrupt
    // only once a channel-3 handler actually exists.
    if (read32(0x800B4394u) != 0) {
        hardware_.complete_dma(3);
        events_.record("dma3");
    }
}

void MipsInterpreter::run_dma4_transfer() {
    // SPU transfers are manual mode: the counter's low half is the word count
    // and zero means 0x10000. The sound data itself is not consumed yet, but
    // the completion flag is: libspu waits for the channel-4 handler to deliver
    // its EvSpu event before it will finish initialising.
    ++dma_words_;
    hardware_.complete_dma(4);
    events_.record("dma4");
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

RunResult MipsInterpreter::run_event_step() {
    stop_after_event_ = true;
    auto result = run(1);
    stop_after_event_ = false;
    return result;
}

RunResult MipsInterpreter::run(std::uint64_t max_steps) {
    RunResult result;

    static const bool trace_walk = std::getenv("R6_TRACE_WALK") != nullptr;
    static const bool trace_sync = std::getenv("R6_TRACE_SYNC") != nullptr;
    static const bool trace_decrypt =
        std::getenv("R6_TRACE_DECRYPT") != nullptr;
    while (result.steps < max_steps) {
        const auto current_pc = cpu_.pc;
        {
            static const bool trace_loop =
                std::getenv("R6_TRACE_LOOP") != nullptr;
            if (trace_loop && current_pc == 0x800934ECu) {
                static int loop_hits = 0;
                if (loop_hits < 4) {
                    ++loop_hits;
                    std::printf("LOOP ra=%08X a0=%08X a1=%08X\n",
                                cpu_.gpr[31], cpu_.gpr[4], cpu_.gpr[5]);
                }
            }
        }
        if (trace_decrypt && current_pc == 0x80010AD0u) {
            // The LCG XOR loader at 0x80010AD0: log its arguments and caller.
            std::printf("DEC %llu dst=%08X src=%08X count=%08X ra=%08X "
                        "cdsectors=%u\n",
                        static_cast<unsigned long long>(hardware_.cycles()),
                        cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[6], cpu_.gpr[31],
                        hardware_.cdrom().sectors_read());
        }
        if (trace_sync &&
            (current_pc == 0x8008A068u || current_pc == 0x80086ABCu ||
             current_pc == 0x80086AB0u || current_pc == 0x80086AB4u)) {
            std::printf("SY %llu pc=%08X a0=%08X a1=%08X ra=%08X v0=%08X "
                        "s0=%08X s1=%08X",
                        static_cast<unsigned long long>(hardware_.cycles()),
                        current_pc, cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[31],
                        cpu_.gpr[2], cpu_.gpr[16], cpu_.gpr[17]);
            std::printf("   cd3C=%08X cd50=%08X cd54=%08X cd58=%08X cd60=%08X "
                        "vs=%08X",
                        read32(0x800B1C3Cu), read32(0x800B1C50u),
                        read32(0x800B1C54u), read32(0x800B1C58u),
                        read32(0x800B1C60u), read32(0x800B397Cu));
            std::fputc(10, stdout);
            std::fputc(10, stdout);
        }
        {
            static const bool trace_table =
                std::getenv("R6_TRACE_TABLE") != nullptr;
            if (trace_table && current_pc >= 0x800BF6E0u &&
                current_pc <= 0x800BF760u) {
                std::printf("TBL pc %08X\n", current_pc);
            }
        }
        if (trace_sync &&
            (current_pc == 0x8008711Cu || current_pc == 0x80086CA8u)) {
            static int pos_hits = 0;
            if (pos_hits < 24) {
                ++pos_hits;
                std::printf("PS %llu pc=%08X a0=%08X a1=%08X ra=%08X",
                            static_cast<unsigned long long>(hardware_.cycles()),
                            current_pc, cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[31]);
                std::fputc(10, stdout);
            }
        }
        if (trace_sync && current_pc == 0x800865C8u) {
            static int par_hits = 0;
            if (par_hits < 8) {
                ++par_hits;
                const auto rec = cpu_.gpr[17];
                std::printf("PA %llu rec=%08X bytes=",
                            static_cast<unsigned long long>(hardware_.cycles()), rec);
                for (int k = 0; k < 8; ++k) {
                    std::printf("%02X", ram_.read8(rec + k));
                }
                std::printf(" a3=%08X s4=%08X", cpu_.gpr[7], cpu_.gpr[20]);
                std::fputc(10, stdout);
            }
        }
        if (trace_sync && current_pc == 0x80086824u) {
            static int lk_hits = 0;
            if (lk_hits < 8) {
                ++lk_hits;
                const auto base = cpu_.gpr[5];
                const auto index = cpu_.gpr[2];
                const auto ea = (base + index - 2372u) & 0x1FFFFFu;
                std::printf("LK %llu base=%08X idx=%08X ea=%08X val=%08X a0=%08X",
                            static_cast<unsigned long long>(hardware_.cycles()),
                            base, index, ea, ram_.read32(ea), cpu_.gpr[4]);
                std::fputc(10, stdout);
            }
        }
        if (trace_sync && current_pc == 0x80086838u) {
            static int rec_hits = 0;
            if (rec_hits < 6) {
                ++rec_hits;
                const auto s0 = cpu_.gpr[16];
                std::printf("WR %llu s0=%08X rec=",
                            static_cast<unsigned long long>(hardware_.cycles()), s0);
                for (int k = 0; k < 8; ++k) {
                    std::printf("%02X", ram_.read8(s0 + k));
                }
                std::printf(" pos=%08X", cpu_.gpr[5]);
                std::fputc(10, stdout);
            }
        }
        if (trace_sync && current_pc == 0x80086A6Cu) {
            static int call_hits = 0;
            if (call_hits < 12) {
                ++call_hits;
                std::printf("RC %llu ra=%08X a0=%08X a1=%08X a2=%08X buf=",
                            static_cast<unsigned long long>(hardware_.cycles()),
                            cpu_.gpr[31], cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[6]);
                const auto at = cpu_.gpr[6];
                for (int k = 0; k < 8; ++k) {
                    std::printf("%02X", ram_.read8(at + k));
                }
                std::fputc(10, stdout);
            }
        }
        if (trace_sync && current_pc == 0x800899A0u) {
            static int fn_hits = 0;
            if (fn_hits < 16) {
                ++fn_hits;
                const auto sp = cpu_.gpr[29];
                std::printf("FN %llu ra=%08X a0=%08X a1=%08X sp=%08X "
                            "cd50=%08X cd60=%08X f0=%08X f1=%08X f2=%08X",
                            static_cast<unsigned long long>(hardware_.cycles()),
                            cpu_.gpr[31], cpu_.gpr[4], cpu_.gpr[5], sp,
                            read32(0x800B1C50u), read32(0x800B1C60u),
                            read32(sp), read32(sp + 4), read32(sp + 8));
                std::fputc(10, stdout);
            }
        }
        if (trace_sync && current_pc == 0x80089AC8u) {
            static int drain_hits = 0;
            if (drain_hits < 12) {
                ++drain_hits;
                std::printf("DR %llu a0=%08X a1=%08X v0=%08X v1=%08X s0=%08X "
                            "ra=%08X cd50=%08X cd5C=%08X cd60=%08X stat=%08X "
                            "irq=%08X dev=%08X",
                            static_cast<unsigned long long>(hardware_.cycles()),
                            cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[2], cpu_.gpr[3],
                            cpu_.gpr[16], cpu_.gpr[31],
                            read32(0x800B1C50u), read32(0x800B1C5Cu),
                            read32(0x800B1C60u), read8(0x1F801800u),
                            read8(0x1F801803u),
                            hardware_.cdrom().interrupt());
                std::fputc(10, stdout);
            }
        }
        if (trace_walk && current_pc == 0x800868B4u) {
            static int walk_records = 0;
            if (walk_records < 200) {
                ++walk_records;
                const auto s0 = cpu_.gpr[16];
                std::printf("WK %llu s0=%08X n=%u bytes=",
                            static_cast<unsigned long long>(hardware_.cycles()),
                            s0, cpu_.gpr[18]);
                for (std::uint32_t i = 0; i < 24; ++i) {
                    std::printf("%02X", read8(s0 + i) & 0xFFu);
                }
                std::fputc(10, stdout);
            }
        }
        for (auto& entry : watched_) {
            if (entry.first == current_pc) ++entry.second;
        }
        for (auto address : watch_args_) {
            if (address != current_pc) continue;
            if (watched_args_log_.size() >= 4096) watched_args_log_.erase(
                watched_args_log_.begin());
            watched_args_log_.push_back(
                "args pc=0x" + format_hex(current_pc) + " ra=0x" +
                format_hex(cpu_.gpr[31]) + " a0=0x" + format_hex(cpu_.gpr[4]) +
                " a1=0x" + format_hex(cpu_.gpr[5]) + " v0=0x" +
                format_hex(cpu_.gpr[2]) + " v1=0x" + format_hex(cpu_.gpr[3]) +
                " sp=0x" + format_hex(cpu_.gpr[29]) + " ret40=0x" +
                format_hex(read32(cpu_.gpr[29] + 40)) + " s0=0x" +
                format_hex(cpu_.gpr[16]) + " s1=0x" + format_hex(cpu_.gpr[17]));
        }
        if (hot_enabled_ && (result.steps & 0x3Fu) == 0) ++hot_[current_pc];
        const bool was_delay_slot = in_delay_slot_;
        const auto target = delay_target_;
        in_delay_slot_ = false;

        if (result.steps % irq_poll_ == 0) {
            // Poll even in a delay slot: a wait loop whose length divides the
            // poll interval would otherwise never see the field counter move.
            // Polling does not touch the PC, so it is safe here.
            if (vblank_enabled_) poll_vblank();
            if (hardware_.pad_tick()) events_.record("pad");
        }

        if (in_interrupt_) {
            ++irq_hot_[current_pc];
            if (++interrupt_steps_ > interrupt_step_budget_) {
                // The routine left its own context, so drop the call and resume
                // the interrupted stream. After a few of these, stop delivering
                // interrupts rather than burning the budget on them.
                if (irq_overrun_traces_.size() < 4 && irq_tail_size_ > 0) {
                    std::vector<std::uint32_t> tail;
                    tail.reserve(irq_tail_size_);
                    const auto start =
                        (irq_tail_next_ + irq_tail_.size() - irq_tail_size_) %
                        irq_tail_.size();
                    for (std::size_t index = 0; index < irq_tail_size_; ++index) {
                        tail.push_back(
                            irq_tail_[(start + index) % irq_tail_.size()]);
                    }
                    irq_overrun_traces_.push_back(std::move(tail));
                    std::vector<std::pair<std::uint32_t, std::uint64_t>> hot(
                        irq_hot_.begin(), irq_hot_.end());
                    std::sort(hot.begin(), hot.end(),
                              [](const auto& left, const auto& right) {
                                  return left.second > right.second;
                              });
                    if (hot.size() > 24) hot.resize(24);
                    irq_overrun_hot_.push_back(std::move(hot));
                    irq_overrun_log_.push_back(
                        "overrun interrupted=0x" + format_hex(cop0_[14]) +
                        " handler_sp=0x" +
                        format_hex(cpu_.gpr[29]) + " steps=" +
                        std::to_string(interrupt_steps_));
                    for (auto site : recent_calls_) {
                        irq_overrun_log_.push_back("  call 0x" + format_hex(site));
                    }
                }
                // Unwind the exception instead of keeping a saved register
                // file: back to EPC with the SR that was pushed, so the
                // interrupted stream is not left with interrupts disabled.
                cpu_.pc = cop0_[14];
                cop0_[12] = interrupt_saved_sr_;
                restore_interrupt_context();
                in_interrupt_ = false;
                if (++interrupt_overruns_ >= kMaxInterruptOverruns) {
                    interrupts_enabled_ = false;
                }
                continue;
            }
        }
        if (!in_interrupt_ && !was_delay_slot && result.steps % irq_poll_ == 0) {
            // Never interrupt a delay slot: the pending branch target lives in
            // the run loop, not in guest state, so resuming mid-pair would
            // lose it.
            deliver_interrupt();
            // The handler jump replaced cpu_.pc, so current_pc is stale: restart
            // the iteration and execute the handler instead of the instruction
            // that was interrupted.
            if (in_interrupt_) {
                if (stop_after_event_) {
                    result.reason = StopReason::step_budget;
                    result.pc = cpu_.pc;
                    return result;
                }
                continue;
            }
        }

        if (current_pc == 0) {
            result.reason = StopReason::returned;
            result.pc = current_pc;
            return result;
        }

        if (service_bios_call()) {
            ++result.steps;
            if (services_.take_return_from_exception() && in_interrupt_) {
                // B(17h) ends the exception the way RFE does, then returns to
                // the instruction EPC kept.
                return_from_exception();
                cpu_.pc = cop0_[14];
                restore_interrupt_context();
                in_interrupt_ = false;
                events_.record("bios_return");
            } else {
                cpu_.pc = cpu_.gpr[31];
            }
            if (recent_pcs_.size() >= 16) recent_pcs_.erase(recent_pcs_.begin());
            recent_pcs_.push_back(current_pc);
            continue;
        }

        const auto instruction = read32(current_pc);
        const auto step = execute(instruction, current_pc);
        if (force_sound_tick_ && current_pc == 0x800340ACu) {
            // 0x800340AC is the tick's `addu $v0, $zero, $zero` return. The
            // boot waits on this value, so report one slot of work here.
            cpu_.gpr[2] = 1;
        }
        ++result.steps;
        hardware_.advance_cycles(kCyclesPerInstruction);
        if (step.returned_from_exception && in_interrupt_) {
            // RFE re-enabled interrupts, so the handler is finished even though
            // the guest has not reached its `jr $k0` yet.
            in_interrupt_ = false;
        }

        if (recent_pcs_.size() >= 16) recent_pcs_.erase(recent_pcs_.begin());
        recent_pcs_.push_back(current_pc);
        if (in_interrupt_ && irq_pcs_.size() < 64) irq_pcs_.push_back(current_pc);
        if (in_interrupt_) {
            irq_tail_[irq_tail_next_] = current_pc;
            irq_tail_next_ = (irq_tail_next_ + 1) % irq_tail_.size();
            if (irq_tail_size_ < irq_tail_.size()) ++irq_tail_size_;
        }
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
            in_delay_slot_ = true;
            delay_target_ = step.target;
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
                case 0x0C:  // syscall: libetc Enter/ExitCriticalSection.
                    // The kernel implements syscall 1 by masking interrupts and
                    // syscall 2 by restoring them. Treating these as no-ops lets
                    // a VBlank land in the middle of a driver section that is
                    // not reentrant, which corrupts the interrupt stack.
                    if (gpr[4] == 1) {
                        // BIOS SYS(01h): clear SR bits 2/10 and report whether
                        // interrupts were enabled.  It is deliberately not a
                        // nesting counter; SYS(02h) always re-enables.
                        cpu_.gpr[2] = (cop0_[12] & 1u) != 0 ? 1u : 0u;
                        cop0_[12] &= ~0x405u;
                    } else if (gpr[4] == 2) {
                        cop0_[12] |= 0x405u;
                    }
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
                return_from_exception();
                events_.record("rfe");
                result.returned_from_exception = true;
            } else {
                result.unsupported = true;
            }
            break;
        case 0x12: {  // COP2 / GTE
            if (rs == 0x00) {  // mfc2
                gpr[rt] = gte_.read_data(rd);
            } else if (rs == 0x02) {  // cfc2
                gpr[rt] = gte_.read_control(rd);
            } else if (rs == 0x04) {  // mtc2
                gte_.write_data(rd, gpr[rt]);
            } else if (rs == 0x06) {  // ctc2
                gte_.write_control(rd, gpr[rt]);
            } else if (rs == 0x08) {
                // BC2F/BC2T test the GTE condition flag, which is always false
                // on hardware: BC2F never branches and BC2T always does.
                result.branch = true;
                result.target =
                    rt == 0
                        ? pc + 8
                        : pc + 4 +
                              (sign_extend16(immediate(instruction)) << 2);
            } else if (rs >= 0x10) {  // GTE command
                ++gte_commands_;
                gte_.execute(instruction);
            } else {
                result.unsupported = true;
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
            gpr[rt] = lwl(gpr[rt], read32(address & ~3u), address);
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
            gpr[rt] = lwr(gpr[rt], read32(address & ~3u), address);
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
            write32(address & ~3u,
                    swl(gpr[rt], read32(address & ~3u), address));
            break;
        }
        case 0x2B:  // sw
            write32(gpr[rs] + sign_extend16(immediate(instruction)), gpr[rt]);
            break;
        case 0x2E: {  // swr
            const auto address = gpr[rs] + sign_extend16(immediate(instruction));
            write32(address & ~3u,
                    swr(gpr[rt], read32(address & ~3u), address));
            break;
        }
        case 0x32:  // lwc2
            gte_.write_data(
                rt, read32(gpr[rs] + sign_extend16(immediate(instruction))));
            break;
        case 0x3A:  // swc2
            write32(gpr[rs] + sign_extend16(immediate(instruction)),
                    gte_.read_data(rt));
            break;
        default:
            result.unsupported = true;
            break;
    }

    return result;
}

}  // namespace r6
