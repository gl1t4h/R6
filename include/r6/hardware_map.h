#pragma once

#include <array>
#include <cstdint>

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
constexpr std::uint32_t kDma0ChannelControl = 0x1F801088;
constexpr std::uint32_t kDma2ChannelControl = 0x1F8010A8;
constexpr std::uint32_t kDmaChannelControlStride = 0x10;
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
    explicit HardwareMap(PlatformServices& services) : services_(services) {}

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
        const auto channel = dma_channel(address);
        if (channel >= 0) {
            return dma_control_[static_cast<std::size_t>(channel)];
        }
        return 0;
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
            // Writing ones acknowledges the matching interrupt bits.
            interrupt_status_ &= ~value;
            return;
        }
        if (address == hardware::kInterruptMask) {
            interrupt_mask_ = value;
            return;
        }
        const auto channel = dma_channel(address);
        if (channel >= 0) {
            // Starting bits are accepted and the transfer is already complete,
            // so the busy and trigger bits read back clear.
            dma_control_[static_cast<std::size_t>(channel)] =
                value & ~(kDmaBusy | kDmaTrigger);
        }
    }

    void raise_interrupt(std::uint32_t bit) { interrupt_status_ |= bit; }

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

    static int dma_channel(std::uint32_t address) {
        constexpr std::uint32_t first = hardware::kDma0ChannelControl;
        constexpr std::uint32_t stride = hardware::kDmaChannelControlStride;
        if (address < first) return -1;
        const auto index = (address - first) / stride;
        if (index >= 7) return -1;
        if ((address - first) % stride != 0) return -1;
        return static_cast<int>(index);
    }

    std::uint32_t timer_value(std::size_t timer) const {
        return (services_.read_timer(timer) - timers_[timer].offset) & 0xFFFFu;
    }

    PlatformServices& services_;
    std::array<Timer, 3> timers_{};
    std::array<std::uint32_t, 7> dma_control_{};
    std::uint32_t interrupt_status_{0};
    std::uint32_t interrupt_mask_{0};
};

}  // namespace r6
