#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "r6/cdrom_device.h"
#include "r6/platform_services.h"

namespace r6 {

// Average cycles one guest instruction takes on the R3000A.
//
// Charging a single cycle made guest software timing loops expire too
// early: libetc's VSync wait counts 32768 iterations of a fourteen
// instruction loop, which at one cycle per instruction finished well
// inside a 564480-cycle frame, so every frame reported "VSync: timeout"
// and the title's frame-driven state machine never advanced. Compiled
// MIPS code retires roughly one instruction per 1.5 to 2 clocks, so a
// uniform cost keeps the guest's software delays longer than a frame
// without pretending any device is slower than it is.
inline constexpr std::uint64_t kCyclesPerInstruction = 2;
namespace hardware {
constexpr std::uint32_t kGpuStatus = 0x1F801814;
constexpr std::uint32_t kGpuCommand = 0x1F801810;
constexpr std::uint32_t kTimer0 = 0x1F801100;
constexpr std::uint32_t kTimer1 = 0x1F801110;
constexpr std::uint32_t kTimer2 = 0x1F801120;
constexpr std::uint32_t kInterruptStatus = 0x1F801070;
constexpr std::uint32_t kInterruptMask = 0x1F801074;
constexpr std::uint32_t kDma0BaseAddress = 0x1F801080;
constexpr std::uint32_t kDma0ChannelControl = 0x1F801088;
constexpr std::uint32_t kDma2BaseAddress = 0x1F8010A0;
constexpr std::uint32_t kDma2ChannelControl = 0x1F8010A8;
constexpr std::uint32_t kDma3BaseAddress = 0x1F8010B0;
constexpr std::uint32_t kDma3ChannelControl = 0x1F8010B8;
constexpr std::uint32_t kDma4BaseAddress = 0x1F8010C0;
constexpr std::uint32_t kDma4ChannelControl = 0x1F8010C8;
constexpr std::uint32_t kDmaInterrupt = 0x1F8010F4;
constexpr std::uint32_t kDmaInterruptBit = 1u << 3;
constexpr std::uint32_t kSpuTransferAddress = 0x1F801DA6;
constexpr std::uint32_t kSpuDataFifo = 0x1F801DA8;
constexpr std::uint32_t kSpuControl = 0x1F801DAA;
constexpr std::uint32_t kSpuTransferControl = 0x1F801DAC;
constexpr std::uint32_t kSpuStatus = 0x1F801DAE;
constexpr std::uint32_t kSpuBase = 0x1F801C00;
constexpr std::uint32_t kSpuEnd = 0x1F801E00;
constexpr std::uint32_t kSpuKeyOn = 0x1F801D88;
constexpr std::uint32_t kSpuKeyOff = 0x1F801D8C;
constexpr std::uint32_t kSpuEndx = 0x1F801D9C;
constexpr std::uint32_t kSpuIrqAddress = 0x1F801DA4;
// I_STAT bit 9 is the SPU, asserted while SPUSTAT bit 6 is set.
constexpr std::uint32_t kInterruptSpu = 1u << 9;
constexpr std::uint32_t kCdromBase = 0x1F801800;
constexpr std::uint32_t kCdromEnd = 0x1F801803;
constexpr std::uint32_t kDmaChannelStride = 0x10;
}  // namespace hardware

// Register file for the devices a booting game polls before it does anything
// interesting: the three root counters, the DMA channel controls and the
// interrupt controller.
//
// Root counters read from the platform clock, because a real one is a
// wall-clock device and library timeouts are written against that. DMA
// transfers complete immediately, which is what a guest sees when it polls the
// channel control register after starting one.
class HardwareMap {
public:
    explicit HardwareMap(PlatformServices& services)
        : services_(services), cdrom_(services) {}

    // CD-ROM registers are byte-wide, so the interpreter routes them before
    // its 32-bit hardware path.
    bool is_cdrom(std::uint32_t physical) const {
        return physical >= hardware::kCdromBase &&
               physical <= hardware::kCdromEnd;
    }
    std::uint8_t read_cdrom(std::uint32_t physical) {
        const auto value = cdrom_.read(physical - hardware::kCdromBase);
        refresh_cdrom_interrupt();
        return value;
    }

    // The disc controller asserts its interrupt line only while a pending
    // flag is also enabled in its own mask register, whether that flag was
    // raised by a command or by the data FIFO running dry.
    void refresh_cdrom_interrupt() {
        const bool pending =
            cdrom_interrupt_enabled_ && cdrom_.interrupt_asserted();
        const bool raised = (interrupt_status_ & kInterruptCdrom) != 0;
        if (pending) {
            interrupt_status_ |= kInterruptCdrom;
            if (!raised) note_irq("cdrom raise", kInterruptCdrom);
        } else {
            interrupt_status_ &= ~kInterruptCdrom;
            if (raised) note_irq("cdrom clear", kInterruptCdrom);
        }
    }
    void write_cdrom(std::uint32_t physical, std::uint8_t value) {
        cdrom_.write(physical - hardware::kCdromBase, value);
        // I_STAT bit 2 is the disc controller. Games route this through the
        // kernel's interrupt callback, so a pending CD interrupt has to show
        // up here or the guest waits forever.
        // The disc interrupt is raised only when the guest has enabled it
        // through the controller's own enable bits, so a stale flag cannot
        // starve the vblank path that drives the frame loop.
        refresh_cdrom_interrupt();
    }
    CdromDevice& cdrom() { return cdrom_; }

    std::uint32_t read32(std::uint32_t address) const {
        if (address == hardware::kTimer0) return timer_value(0);
        if (address == hardware::kTimer1) return timer_value(1);
        if (address == hardware::kTimer2) return timer_value(2);
        for (std::size_t timer = 0; timer < timers_.size(); ++timer) {
            const auto base = hardware::kTimer0 + timer * 0x10u;
            if (address == base + 4) return timers_[timer].mode;
            if (address == base + 8) return timers_[timer].target;
        }
        if (address == hardware::kInterruptStatus) return interrupt_status_;
        if (address == hardware::kInterruptMask) return interrupt_mask_;
        if (address == hardware::kGpuStatus) {
            // GPUSTAT bit 31 (ODE) is the drawing field: it toggles per field
            // and reads zero throughout vertical blank. Bit 22 is the display
            // colour depth set by GP1(08h), not an interlace flag, so it is
            // left to the GPU model instead of following the field.
            std::uint32_t value = services_.read_gpu_status();
            if (gpu_field_ && gpu_vblank_cycles_ == 0) {
                value |= 0x80000000u;
            }
            return value;
        }
        if (address == hardware::kGpuCommand) return services_.read_gpu_data();
        if (address == kPadData) {
            pad_byte_received();
            return pad_data_;
        }
        if (address == kPadStat) return kPadReadyStatus;
        if (address == kPadMode) {
            return pad_mode_ | (pad_control_ << 16);
        }
        if (address == kPadBaud) return pad_baud_ << 16;
        const auto channel = dma_register(address);
        if (channel >= 0) {
            const auto slot = static_cast<std::size_t>(channel);
            if (channel % 4 == 0) return dma_address_[slot / 4];
            if (channel % 4 == 1) return dma_counter_[slot / 4];
            return dma_control_[slot / 4];
        }
        if (address == hardware::kDmaInterrupt) return dma_interrupt_;
        if (address >= hardware::kSpuBase && address < hardware::kSpuEnd) {
            return read_spu(address);
        }
        return 0;
    }

    // DMA channel base address, word counter and control register, for the
    // interpreter that executes linked lists.
    std::uint32_t dma_address(std::size_t channel) const {
        return dma_address_[channel] & 0x00FFFFFFu;
    }
    std::uint32_t dma_counter(std::size_t channel) const {
        return dma_counter_[channel];
    }
    std::uint32_t dma_control(std::size_t channel) const {
        return dma_control_[channel];
    }

    void write32(std::uint32_t address, std::uint32_t value) {
        if (address >= hardware::kSpuBase && address < hardware::kSpuEnd) {
            // The register block is 16 bits wide, so a word store covers two
            // registers (key on/off are written that way).
            write_spu16(address, value & 0xFFFFu);
            write_spu16(address + 2u, (value >> 16) & 0xFFFFu);
            return;
        }
        if (address == hardware::kGpuCommand ||
            address == hardware::kGpuStatus) {
            services_.write_gpu_register(address, value);
            return;
        }
        for (std::size_t timer = 0; timer < timers_.size(); ++timer) {
            const auto base = hardware::kTimer0 + timer * 0x10u;
            if (address == base) {
                // Writing the counter resets its time base.
                timers_[timer].offset = static_cast<std::uint32_t>(cycles_);
                return;
            }
            if (address == base + 4) {
                // Writing a mode register also restarts its counter.
                timers_[timer].mode = value;
                timers_[timer].offset = static_cast<std::uint32_t>(cycles_);
                return;
            }
            if (address == base + 8) {
                timers_[timer].target = value;
                return;
            }
        }
        if (address == hardware::kInterruptStatus) {
            // PS1 I_STAT is acknowledged by writing a zero to a bit: one means
            // "no change". Guest kernel code stores ~(1 << irq) to clear one
            // source without touching the others.
            const auto cleared = interrupt_status_ & ~value;
            interrupt_status_ &= value;
            if (cleared != 0) note_irq("guest ack", cleared);
            return;
        }
        if (address == hardware::kInterruptMask) {
            interrupt_mask_ = value;
            return;
        }
        if (address == kPadData) {
            pad_data_ = value & 0xFFFFu;
            // A byte takes time to shift out on real hardware, so raise the
            // controller interrupt a little later: raising it immediately lets
            // the driver acknowledge it before its own wait loop starts.
            pad_irq_delay_ = 4;
            return;
        }
        if (address == kPadMode) {
            pad_mode_ = value & 0xFFFFu;
            pad_control_ = value >> 16;
            return;
        }
        if (address == kPadBaud) {
            pad_baud_ = value >> 16;
            return;
        }
        const auto channel = dma_register(address);
        if (channel >= 0) {
            const auto slot = static_cast<std::size_t>(channel / 4);
            switch (channel % 4) {
                case 0:
                    dma_address_[slot] = value & 0x00FFFFFFu;
                    return;
                case 1:
                    dma_counter_[slot] = value;
                    return;
                default:
                    // Starting bits are accepted and the transfer completes
                    // before the guest can poll, so busy and trigger read
                    // back clear.
                    dma_control_[slot] = value & ~(kDmaBusy | kDmaTrigger);
                    return;
            }
        }
        if (address == hardware::kDmaInterrupt) {
            // DICR channel flags (bits 24-30) are acknowledged by writing a
            // one, unlike I_STAT where a zero clears.  Lower bits are the
            // writable channel-control fields; bit 31 is derived.
            constexpr std::uint32_t kFlagMask = 0x7F000000u;
            dma_interrupt_ &= ~(value & kFlagMask);
            dma_interrupt_ =
                (dma_interrupt_ & kFlagMask) | (value & 0x00FFFFFFu);
            return;
        }
    }

    // Halfword write, used by the interpreters' 16-bit store path so a sh does
    // not overwrite the neighbouring register.
    void write_spu16(std::uint32_t address, std::uint32_t value) {
        const auto index = (address - hardware::kSpuBase) / 2u;
        spu_write_register(index, static_cast<std::uint16_t>(value));
    }

    // Copies one DMA4 word into sound RAM at the transfer cursor. The guest
    // uploads ADPCM data this way before keying a voice on.
    void write_spu_ram_word(std::uint32_t value) {
        const auto address = spu_transfer_cursor_ & kSpuAddressMask;
        spu_ram_[address >> 1] = static_cast<std::uint16_t>(value);
        spu_ram_[((address + 2u) & kSpuAddressMask) >> 1] =
            static_cast<std::uint16_t>(value >> 16);
        spu_dma_words_ += 2u;
        spu_transfer_cursor_ = (address + 4u) & kSpuAddressMask;
        spu_transfer_address_ = (spu_transfer_cursor_ >> 3) & 0xFFFFu;
    }

    bool spu_voice_active(std::size_t voice) const {
        return voice < spu_voices_.size() && spu_voices_[voice].active;
    }
    std::uint32_t spu_endx() const { return spu_endx_; }
    // Bring-up counters: distinguish "the game never touches the SPU" from
    // "the model saw the writes and still did not change the outcome".
    std::uint64_t spu_register_writes() const { return spu_register_writes_; }
    std::uint64_t spu_key_ons() const { return spu_key_ons_; }
    std::uint64_t spu_key_offs() const { return spu_key_offs_; }
    std::uint64_t spu_voice_ends() const { return spu_voice_ends_; }
    std::uint64_t spu_irqs() const { return spu_irqs_; }
    std::uint64_t spu_dma_words() const { return spu_dma_words_; }
    // Voices whose ADPCM walk found a LOOP-END flag, and the first key-on's
    // start address and sample count, so an empty walk can be told apart from
    // a driver that simply never uploads sample data.
    std::uint64_t spu_walks_with_end() const { return spu_walks_with_end_; }
    std::uint32_t spu_first_start() const { return spu_first_start_; }
    std::uint32_t spu_first_samples() const { return spu_first_samples_; }

    // Called when a DMA channel finishes. The channel flag in 0x1F8010F4 is
    // what the guest's DMA handler dispatches on, and the DMA interrupt is
    // I_STAT bit 3.
    void complete_dma(std::size_t channel) {
        dma_interrupt_ |= 1u << (24 + channel);
        raise_interrupt(hardware::kDmaInterruptBit);
    }

    // Delays a channel's completion by a transfer-time estimate. The channel
    // reads back busy until then. `interrupt` selects whether the completion
    // pulls I_STAT bit 3 as well as setting its DICR flag.
    void schedule_dma_completion(std::size_t channel, std::uint64_t delay,
                                 bool interrupt) {
        dma_control_[channel] |= kDmaBusy;
        for (auto& pending : pending_dma_) {
            if (pending.channel == channel) {
                pending.due = cycles_ + delay;
                pending.interrupt = interrupt;
                return;
            }
        }
        pending_dma_.push_back(PendingDma{channel, cycles_ + delay, interrupt});
    }

    // A DMA interrupt must be taken at the instruction where the transfer
    // finished, not at the next periodic poll: the CD stream's callback has
    // to run in the same post-tick window that produced its record, before
    // the next tick advances the produced count. Returns and clears the
    // channels whose interrupt is ready.
    std::uint32_t take_dma_completion() {
        const auto ready = pending_dma_ready_;
        pending_dma_ready_ = 0;
        return ready;
    }

    void raise_interrupt(std::uint32_t bit) {
        const bool raised = (interrupt_status_ & bit) != 0;
        interrupt_status_ |= bit;
        if (!raised) note_irq("raise", bit);
    }

    [[nodiscard]] std::uint64_t cycles() const { return cycles_; }
    // Each executed instruction advances the root counters, so a run is
    // reproducible: a host-clock timer made CD timeouts depend on how fast the
    // interpreter happened to be running.
    void advance_cycles(std::uint64_t count) {
        cycles_ += count;
        // A DMA channel stays busy for the transfer's duration. Completing it
        // here, rather than on the triggering store, lets a guest that polls
        // CHCR see the transfer take time and keeps its completion interrupt
        // ordered after the code that started it.
        for (auto it = pending_dma_.begin(); it != pending_dma_.end();) {
            if (cycles_ < it->due) {
                ++it;
                continue;
            }
            dma_control_[it->channel] &= ~kDmaBusy;
            dma_interrupt_ |= 1u << (24 + it->channel);
            if (it->interrupt) {
                raise_interrupt(hardware::kDmaInterruptBit);
                pending_dma_ready_ |= 1u << it->channel;
            }
            it = pending_dma_.erase(it);
        }
        // Voices are advanced in batches; a per-instruction update would cost
        // more than the rest of the device model together.
        spu_tick_cycles_ += count;
        if (spu_tick_cycles_ >= kSpuTickCycles) {
            spu_tick_cycles_ = 0;
            tick_spu();
        }
        if (gpu_vblank_cycles_ > count) {
            gpu_vblank_cycles_ -= count;
        } else {
            gpu_vblank_cycles_ = 0;
        }
        // A deferred CD command raises its acknowledgement here rather than
        // on the store, so the interrupt line has to be refreshed when it
        // fires, not only on a register access.
        if (cdrom_.advance_cycles(count)) refresh_cdrom_interrupt();
    }

    // A field starts with vertical blank, where GPUSTAT ODE reads zero, then
    // shows its own even/odd polarity until the next field.
    void toggle_gpu_field() {
        gpu_field_ = !gpu_field_;
        gpu_vblank_cycles_ = kGpuVblankCycles;
    }
    // The kernel acknowledges a serviced interrupt by clearing its I_STAT bit.
    // Called from the interpreter's periodic poll: delivers the pad transfer
    // interrupt once the modelled shift-out delay has elapsed.
    bool pad_tick() {
        if (pad_irq_delay_ == 0) return false;
        if (--pad_irq_delay_ == 0) {
            raise_interrupt(kInterruptPad);
            return true;
        }
        return false;
    }

    // A byte shifted in from the controller raises the same interrupt as one
    // shifted out, and the driver waits on it after its final transfer.
    void pad_byte_received() const { pad_irq_delay_ = 4; }

    // Bounded interrupt-controller event log for bring-up.
    const std::vector<std::string>& interrupt_log() const {
        return interrupt_log_;
    }

    // Pad / SIO0 registers. A transfer must look complete, otherwise the
    // guest's pad driver spins on JOY_STAT until its timer-based timeout.
    static constexpr std::uint32_t kPadData = 0x1F801040;
    static constexpr std::uint32_t kPadStat = 0x1F801044;
    static constexpr std::uint32_t kPadMode = 0x1F801048;
    static constexpr std::uint32_t kPadBaud = 0x1F80104C;
    static constexpr std::uint32_t kPadReadyStatus = 0x0007;

    void set_cdrom_interrupt_enabled(bool enabled) {
        cdrom_interrupt_enabled_ = enabled;
    }

    static constexpr std::uint32_t kInterruptVblank = 1u << 0;
    static constexpr std::uint32_t kInterruptCdrom = 1u << 2;
    static constexpr std::uint32_t kInterruptPad = 1u << 7;

    std::uint32_t interrupt_status() const { return interrupt_status_; }
    std::uint32_t interrupt_mask() const { return interrupt_mask_; }

    // Full guest-visible hardware state, excluding diagnostic logs and the
    // platform service reference. Timer values are derived from cycles_ and
    // the timer registers, so comparing both covers the timer reads too.
    bool same_state(const HardwareMap& other) const {
        return state_difference(other).empty();
    }

    // Names the first divergent hardware group, for a failed differential.
    std::string state_difference(const HardwareMap& other) const {
        if (!cdrom_.same_state(other.cdrom_)) return "cdrom";
        if (cycles_ != other.cycles_) return "cycles";
        if (dma_interrupt_ != other.dma_interrupt_) return "dma interrupt";
        if (spu_transfer_address_ != other.spu_transfer_address_)
            return "spu transfer address";
        if (spu_control_ != other.spu_control_) return "spu control";
        if (spu_transfer_control_ != other.spu_transfer_control_)
            return "spu transfer control";
        if (dma_address_ != other.dma_address_) return "dma address";
        if (dma_counter_ != other.dma_counter_) return "dma counter";
        if (dma_control_ != other.dma_control_) return "dma control";
        if (pad_data_ != other.pad_data_) return "pad data";
        if (pad_irq_delay_ != other.pad_irq_delay_) return "pad irq delay";
        if (pad_mode_ != other.pad_mode_) return "pad mode";
        if (pad_control_ != other.pad_control_) return "pad control";
        if (pad_baud_ != other.pad_baud_) return "pad baud";
        if (interrupt_status_ != other.interrupt_status_)
            return "interrupt status";
        if (cdrom_interrupt_enabled_ != other.cdrom_interrupt_enabled_)
            return "cdrom interrupt enable";
        if (gpu_field_ != other.gpu_field_) return "gpu field";
        if (gpu_vblank_cycles_ != other.gpu_vblank_cycles_)
            return "gpu vblank cycles";
        if (interrupt_mask_ != other.interrupt_mask_) return "interrupt mask";
        for (std::size_t index = 0; index < timers_.size(); ++index) {
            if (timers_[index].mode != other.timers_[index].mode)
                return "timer " + std::to_string(index) + " mode";
            if (timers_[index].target != other.timers_[index].target)
                return "timer " + std::to_string(index) + " target";
            if (timers_[index].offset != other.timers_[index].offset)
                return "timer " + std::to_string(index) + " offset";
        }
        return {};
    }

private:
    // The SPU is modelled far enough to let a driver release a voice: the
    // 512 KiB sound RAM fed by DMA4, the voice register file, key on/off,
    // ENDX and the IRQ address. Samples are not mixed; a voice's position is
    // derived from the cycles since key-on scaled by its pitch, and its end
    // from the ADPCM block headers in sound RAM.
    static constexpr std::uint32_t kSpuAddressMask = 0x7FFFFu;
    static constexpr std::uint32_t kSpuRamMask = 0x3FFFFu;
    static constexpr std::uint32_t kSpuSize = 0x80000u;
    static constexpr std::uint32_t kSpuVoiceCount = 24u;
    static constexpr std::uint32_t kSpuVoiceBytes = 0x180u;
    static constexpr std::uint32_t kAdpcmBlockBytes = 16u;
    static constexpr std::uint32_t kAdpcmBlockSamples = 28u;
    static constexpr std::uint32_t kAdpcmBlockLimit = 0x8000u;
    static constexpr std::uint32_t kSpuSampleRate = 44100u;
    static constexpr std::uint64_t kSpuClock = 33868800u;
    static constexpr std::uint32_t kPitchOne = 0x1000u;
    static constexpr std::uint32_t kSpuEnable = 0x8000u;
    static constexpr std::uint32_t kSpuIrqEnable = 0x0040u;
    static constexpr std::uint64_t kSpuTickCycles = 512u;

    struct SpuVoice {
        std::array<std::uint16_t, 8> registers{};
        bool active{false};
        bool loops{false};
        std::uint64_t key_on_cycle{0};
        std::uint32_t pitch{kPitchOne};
        std::uint32_t start_address{0};
        std::uint32_t end_address{0};
        std::uint32_t total_samples{0};
        std::uint32_t current_address{0};
    };

    std::uint32_t read_spu(std::uint32_t address) const {
        const auto index = (address - hardware::kSpuBase) / 2u;
        return spu_register(index) |
               (static_cast<std::uint32_t>(spu_register(index + 1)) << 16);
    }

    std::uint16_t spu_register(std::uint32_t index) const {
        const auto offset = index * 2u;
        if (offset < kSpuVoiceBytes) {
            const auto voice = offset / 0x10u;
            const auto reg = (offset % 0x10u) / 2u;
            const auto& state = spu_voices_[voice];
            // Current ADSR volume reads as full while the voice plays, zero
            // once it has ended, which is what a driver polls to see life.
            if (reg == 6) return state.active ? 0x7FFFu : 0u;
            return state.registers[reg];
        }
        if (offset < 0x180u || offset >= 0x180u + 0x80u) return 0;
        switch (offset) {
            case 0x19Cu:
                return static_cast<std::uint16_t>(spu_endx_);
            case 0x19Eu:
                return static_cast<std::uint16_t>(spu_endx_ >> 16);
            case 0x1A4u:
                return static_cast<std::uint16_t>(spu_irq_address_);
            case 0x1A6u:
                return static_cast<std::uint16_t>(spu_transfer_address_);
            case 0x1AAu:
                return static_cast<std::uint16_t>(spu_control_);
            case 0x1ACu:
                return static_cast<std::uint16_t>(spu_transfer_control_);
            case 0x1AEu:
                return spu_irq_pending_ ? 0x0040u : 0u;
            default:
                break;
        }
        return spu_registers_[(offset - 0x180u) / 2u];
    }

    void spu_write_register(std::uint32_t index, std::uint16_t value) {
        ++spu_register_writes_;
        const auto offset = index * 2u;
        if (offset < kSpuVoiceBytes) {
            const auto voice = offset / 0x10u;
            const auto reg = (offset % 0x10u) / 2u;
            auto& state = spu_voices_[voice];
            state.registers[reg] = value;
            if (reg == 2) state.pitch = value == 0 ? kPitchOne : value;
            if (reg == 3) state.start_address = value * 8u;
            return;
        }
        if (offset < 0x180u || offset >= 0x180u + 0x80u) return;
        switch (offset) {
            case 0x188u:
                spu_key_on(value);
                return;
            case 0x18Au:
                spu_key_on(static_cast<std::uint32_t>(value) << 16);
                return;
            case 0x18Cu:
                spu_key_off(value);
                return;
            case 0x18Eu:
                spu_key_off(static_cast<std::uint32_t>(value) << 16);
                return;
            case 0x1A4u:
                spu_irq_address_ = value & 0xFFFFu;
                return;
            case 0x1A6u:
                spu_transfer_address_ = value & 0xFFFFu;
                spu_transfer_cursor_ = (value & 0xFFFFu) * 8u;
                return;
            case 0x1A8u:
                spu_write_fifo(value);
                return;
            case 0x1AAu:
                write_spu_control(value);
                return;
            case 0x1ACu:
                spu_transfer_control_ = value;
                return;
            default:
                spu_registers_[(offset - 0x180u) / 2u] = value;
                return;
        }
    }

    void spu_write_fifo(std::uint16_t value) {
        const auto address = spu_transfer_cursor_ & kSpuAddressMask;
        spu_ram_[address >> 1] = value;
        spu_transfer_cursor_ = (address + 2u) & kSpuAddressMask;
        spu_transfer_address_ = (spu_transfer_cursor_ >> 3) & 0xFFFFu;
    }

    void write_spu_control(std::uint16_t value) {
        const bool was_enabled = (spu_control_ & kSpuEnable) != 0;
        spu_control_ = value;
        // Clearing bit 6 acknowledges the IRQ: the level drops with SPUSTAT
        // bit 6 and I_STAT bit 9 clears with it.
        if ((value & kSpuIrqEnable) == 0 && spu_irq_pending_) {
            spu_irq_pending_ = false;
            interrupt_status_ &= ~hardware::kInterruptSpu;
        }
        if (was_enabled && (value & kSpuEnable) == 0) {
            for (auto& voice : spu_voices_) voice.active = false;
        }
    }

    void spu_key_on(std::uint32_t mask) {
        for (std::uint32_t voice = 0; voice < kSpuVoiceCount; ++voice) {
            if ((mask & (1u << voice)) == 0) continue;
            auto& state = spu_voices_[voice];
            ++spu_key_ons_;
            state.active = true;
            state.key_on_cycle = cycles_;
            state.start_address =
                static_cast<std::uint32_t>(state.registers[3]) * 8u;
            state.current_address = state.start_address;
            state.pitch =
                state.registers[2] == 0 ? kPitchOne : state.registers[2];
            // ENDX clears for a voice when it is keyed on.
            spu_endx_ &= ~(1u << voice);
            walk_adpcm(state);
            if (spu_key_ons_ == 1) {
                spu_first_start_ = state.start_address;
                spu_first_samples_ = state.total_samples;
            }
            if (state.total_samples != 0) ++spu_walks_with_end_;
        }
    }

    void spu_key_off(std::uint32_t mask) {
        for (std::uint32_t voice = 0; voice < kSpuVoiceCount; ++voice) {
            if ((mask & (1u << voice)) != 0) {
                if (spu_voices_[voice].active) ++spu_key_offs_;
                spu_voices_[voice].active = false;
            }
        }
    }

    // Scans the ADPCM blocks from the voice's start address for the LOOP-END
    // flag, so the voice knows how many samples it can play before it ends.
    void walk_adpcm(SpuVoice& voice) {
        auto address = voice.start_address & kSpuAddressMask;
        std::uint32_t samples = 0;
        voice.total_samples = 0;
        voice.loops = false;
        for (std::uint32_t block = 0; block < kAdpcmBlockLimit; ++block) {
            const auto header = spu_ram_[(address >> 1) & kSpuRamMask];
            const auto flags = static_cast<std::uint8_t>(header >> 8);
            samples += kAdpcmBlockSamples;
            address = (address + kAdpcmBlockBytes) & kSpuAddressMask;
            if ((flags & 0x01u) == 0) continue;
            voice.total_samples = samples;
            voice.end_address = address;
            voice.loops = (flags & 0x02u) != 0;
            return;
        }
        // No end marker in the uploaded data yet: hold the voice until it is
        // keyed off, as a real SPU would keep reading whatever is in RAM.
        voice.end_address = address;
    }

    void tick_spu() {
        for (std::uint32_t voice = 0; voice < kSpuVoiceCount; ++voice) {
            auto& state = spu_voices_[voice];
            if (!state.active) continue;
            const auto elapsed = cycles_ - state.key_on_cycle;
            auto played = elapsed * kSpuSampleRate *
                          static_cast<std::uint64_t>(state.pitch) /
                          (kSpuClock * kPitchOne);
            if (state.total_samples != 0 &&
                played >= state.total_samples) {
                spu_endx_ |= 1u << voice;
                if (!state.loops) {
                    ++spu_voice_ends_;
                    state.active = false;
                    state.current_address = state.end_address;
                    continue;
                }
                played %= state.total_samples;
            }
            const auto block = played / kAdpcmBlockSamples;
            state.current_address =
                (state.start_address +
                 static_cast<std::uint32_t>(block) * kAdpcmBlockBytes) &
                kSpuAddressMask;
        }
        poll_spu_irq();
    }

    void poll_spu_irq() {
        if ((spu_control_ & kSpuEnable) == 0) return;
        if ((spu_control_ & kSpuIrqEnable) == 0) return;
        if (spu_irq_pending_) return;
        const auto target = (spu_irq_address_ * 8u) & kSpuAddressMask;
        for (const auto& voice : spu_voices_) {
            if (!voice.active) continue;
            if ((voice.current_address & ~7u) == (target & ~7u)) {
                spu_irq_pending_ = true;
                ++spu_irqs_;
                raise_interrupt(hardware::kInterruptSpu);
                return;
            }
        }
    }

    static constexpr std::uint32_t kDmaBusy = 1u << 24;
    static constexpr std::uint32_t kDmaTrigger = 1u << 28;
    // 33.8688 MHz / 60 fields / 263 scanlines.
    static constexpr std::uint64_t kHBlankCycles = 2146;
    // Vertical retrace and the upper/lower borders, about 20 scanlines.
    static constexpr std::uint64_t kGpuVblankCycles = kHBlankCycles * 20;

    struct PendingDma {
        std::size_t channel;
        std::uint64_t due;
        bool interrupt;
    };

    struct Timer {
        std::uint32_t mode{0};
        std::uint32_t target{0};
        std::uint32_t offset{0};
    };

    // Index of a DMA register within the block: channel * 4 + register, where
    // register 0 is the base address, 1 the word counter and 2 the control.
    static int dma_register(std::uint32_t address) {
        constexpr std::uint32_t first = hardware::kDma0BaseAddress;
        constexpr std::uint32_t stride = hardware::kDmaChannelStride;
        if (address < first) return -1;
        const auto offset = address - first;
        if (offset % 4 != 0) return -1;
        const auto index = offset / stride;
        const auto within = (offset % stride) / 4;
        if (index >= 7 || within > 2) return -1;
        return static_cast<int>(index * 4 + within);
    }

    std::uint32_t timer_value(std::size_t timer) const {
        // Bits 8-9 select the clock: timer 0 counts sysclk or the dot clock,
        // timer 1 sysclk or hblank, and timer 2 sysclk or sysclk/8 (selected
        // by bit 1).  The dot clock depends on the display mode, which this
        // map does not track, so it falls back to sysclk.
        const auto source = (timers_[timer].mode >> 8) & 3u;
        const std::uint64_t elapsed = cycles_ - timers_[timer].offset;
        if (timer == 1 && (source & 1u) != 0) {
            // One tick per scanline (~2146 CPU cycles).  Sony's VSync/CD
            // timeouts count these, and an "until two reads agree" loop would
            // land on the same phase for ever at the wrong rate.
            return static_cast<std::uint32_t>(elapsed / kHBlankCycles) &
                   0xFFFFu;
        }
        if (timer == 2 && (source & 2u) != 0) {
            return static_cast<std::uint32_t>(elapsed / 8u) & 0xFFFFu;
        }
        return static_cast<std::uint32_t>(elapsed) & 0xFFFFu;
    }

    void note_irq(const char* what, std::uint32_t bits) {
        // Keep the first events rather than the most recent: bring-up problems
        // usually happen during initialisation, long before the run ends.
        if (interrupt_log_.size() >= 96) return;
        interrupt_log_.push_back(std::string(what) + " bits=" +
                                 std::to_string(bits) + " stat=" +
                                 std::to_string(interrupt_status_) + " mask=" +
                                 std::to_string(interrupt_mask_));
    }

    PlatformServices& services_;
    CdromDevice cdrom_;
    std::uint64_t cycles_{0};
    std::uint32_t dma_interrupt_{0};
    std::uint32_t spu_transfer_address_{0};
    std::uint32_t spu_control_{0};
    std::uint32_t spu_transfer_control_{0};
    // Sound RAM lives on the heap: half a megabyte by value would overflow
    // the host stack in the small test drivers.
    std::vector<std::uint16_t> spu_ram_ =
        std::vector<std::uint16_t>(kSpuSize / 2, 0u);
    std::array<SpuVoice, kSpuVoiceCount> spu_voices_{};
    std::array<std::uint16_t, 0x40> spu_registers_{};
    std::uint32_t spu_transfer_cursor_{0};
    std::uint32_t spu_irq_address_{0};
    std::uint32_t spu_endx_{0};
    bool spu_irq_pending_{false};
    std::uint64_t spu_tick_cycles_{0};
    std::uint64_t spu_register_writes_{0};
    std::uint64_t spu_key_ons_{0};
    std::uint64_t spu_key_offs_{0};
    std::uint64_t spu_walks_with_end_{0};
    std::uint32_t spu_first_start_{0};
    std::uint32_t spu_first_samples_{0};
    std::uint64_t spu_voice_ends_{0};
    std::uint64_t spu_irqs_{0};
    std::uint64_t spu_dma_words_{0};
    std::array<Timer, 3> timers_{};
    std::array<std::uint32_t, 7> dma_address_{};
    std::array<std::uint32_t, 7> dma_counter_{};
    std::array<std::uint32_t, 7> dma_control_{};
    std::vector<PendingDma> pending_dma_;
    std::uint32_t pending_dma_ready_{0};
    std::uint32_t pad_data_{0x41};
    mutable std::uint32_t pad_irq_delay_{0};
    std::uint32_t pad_mode_{0};
    std::uint32_t pad_control_{0};
    std::uint32_t pad_baud_{0};
    std::uint32_t interrupt_status_{0};
    bool cdrom_interrupt_enabled_{false};
    bool gpu_field_{false};
    std::uint64_t gpu_vblank_cycles_{0};
    std::uint32_t interrupt_mask_{0};
    std::vector<std::string> interrupt_log_;
};

}  // namespace r6
