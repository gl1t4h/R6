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
        if (address == hardware::kGpuStatus) return services_.read_gpu_status();
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
                timers_[timer].offset = services_.read_timer(timer);
                return;
            }
            if (address == base + 4) {
                timers_[timer].mode = value;
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
    }

    void raise_interrupt(std::uint32_t bit) {
        const bool raised = (interrupt_status_ & bit) != 0;
        interrupt_status_ |= bit;
        if (!raised) note_irq("raise", bit);
    }
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

    void acknowledge_interrupts(std::uint32_t bits) {
        const auto cleared = interrupt_status_ & bits;
        interrupt_status_ &= ~bits;
        if (cleared != 0) note_irq("service ack", cleared);
    }

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
        return (services_.read_timer(timer) - timers_[timer].offset) & 0xFFFFu;
    }

    void note_irq(const char* what, std::uint32_t bits) {
        if (interrupt_log_.size() >= 64) interrupt_log_.erase(interrupt_log_.begin());
        interrupt_log_.push_back(std::string(what) + " bits=" +
                                 std::to_string(bits) + " stat=" +
                                 std::to_string(interrupt_status_) + " mask=" +
                                 std::to_string(interrupt_mask_));
    }

    PlatformServices& services_;
    CdromDevice cdrom_;
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
    std::uint32_t interrupt_mask_{0};
    std::vector<std::string> interrupt_log_;
};

}  // namespace r6
