#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "r6/cdrom_device.h"
#include "r6/platform_services.h"

namespace r6 {

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

    // The disc controller asserts its interrupt line whenever it has an
    // interrupt flag pending, whether that flag was raised by a command or by
    // the data FIFO running dry.
    void refresh_cdrom_interrupt() {
        const bool pending = cdrom_.interrupt() != 0;
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
            // GPUSTAT bit 22 is the interlace field and bit 31 the even/odd
            // line. Sony's VSync() spins until bit 31 changes while bit 22 is
            // set, so a model that never flips them hangs the guest inside its
            // first timer wait.
            std::uint32_t value = services_.read_gpu_status();
            if (gpu_field_) value |= 0x80400000u;
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
        if (address == hardware::kSpuTransferAddress) {
            return spu_transfer_address_;
        }
        if (address == hardware::kSpuControl) return spu_control_;
        if (address == hardware::kSpuTransferControl) {
            return spu_transfer_control_;
        }
        if (address == hardware::kSpuStatus) {
            // SPU status flags (DMA busy, IRQ9) are not modelled. Reporting
            // "idle" lets libspu's "wait for DMAF clear" loops finish, which
            // is what a real SPU does once the transfer has landed.
            return 0;
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
            // Channel flags are acknowledged by writing a zero, like I_STAT.
            dma_interrupt_ &= value | ~0x7F000000u;
            return;
        }
        if (address == hardware::kSpuTransferAddress) {
            spu_transfer_address_ = value & 0xFFFFu;
            return;
        }
        if (address == hardware::kSpuDataFifo) {
            // FIFO contents are not used yet. libspu checks that the transfer
            // address it programmed is still what the status register reports
            // after a manual write, so leave the address alone here.
            return;
        }
        if (address == hardware::kSpuControl) {
            spu_control_ = value & 0xFFFFu;
            return;
        }
        if (address == hardware::kSpuTransferControl) {
            spu_transfer_control_ = value & 0xFFFFu;
            return;
        }
    }

    // Called when a DMA channel finishes. The channel flag in 0x1F8010F4 is
    // what the guest's DMA handler dispatches on, and the DMA interrupt is
    // I_STAT bit 3.
    void complete_dma(std::size_t channel) {
        dma_interrupt_ |= 1u << (24 + channel);
        raise_interrupt(hardware::kDmaInterruptBit);
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
    void advance_cycles(std::uint64_t count) { cycles_ += count; }

    // Each field flips the interlace and even/odd-line bits in GPUSTAT.
    void toggle_gpu_field() { gpu_field_ = !gpu_field_; }
    // The kernel acknowledges a serviced interrupt by clearing its I_STAT bit.
    // Called from the interpreter's periodic poll: delivers the pad transfer
    // interrupt once the modelled shift-out delay has elapsed.
    void pad_tick() {
        if (pad_irq_delay_ == 0) return;
        if (--pad_irq_delay_ == 0) raise_interrupt(kInterruptPad);
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

private:
    static constexpr std::uint32_t kDmaBusy = 1u << 24;
    static constexpr std::uint32_t kDmaTrigger = 1u << 28;
    // 33.8688 MHz / 60 fields / 263 scanlines.
    static constexpr std::uint64_t kHBlankCycles = 2146;

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
    std::array<Timer, 3> timers_{};
    std::array<std::uint32_t, 7> dma_address_{};
    std::array<std::uint32_t, 7> dma_counter_{};
    std::array<std::uint32_t, 7> dma_control_{};
    std::uint32_t pad_data_{0x41};
    mutable std::uint32_t pad_irq_delay_{0};
    std::uint32_t pad_mode_{0};
    std::uint32_t pad_control_{0};
    std::uint32_t pad_baud_{0};
    std::uint32_t interrupt_status_{0};
    bool cdrom_interrupt_enabled_{false};
    bool gpu_field_{false};
    std::uint32_t interrupt_mask_{0};
    std::vector<std::string> interrupt_log_;
};

}  // namespace r6
