#pragma once

#include <cstdint>

#include "r6/platform_services.h"

namespace r6 {

namespace hardware {
constexpr std::uint32_t kGpuStatus = 0x1F801814;
constexpr std::uint32_t kGpuCommand = 0x1F801810;
constexpr std::uint32_t kTimer0 = 0x1F801100;
constexpr std::uint32_t kTimer1 = 0x1F801110;
constexpr std::uint32_t kTimer2 = 0x1F801120;
}  // namespace hardware

class HardwareMap {
public:
    explicit HardwareMap(PlatformServices& services) : services_(services) {}

    std::uint32_t read32(std::uint32_t address) const {
        if (address == hardware::kTimer0) return services_.read_timer(0);
        if (address == hardware::kTimer1) return services_.read_timer(1);
        if (address == hardware::kTimer2) return services_.read_timer(2);
        return 0;
    }

    void write32(std::uint32_t address, std::uint32_t value) {
        if (address == hardware::kGpuCommand ||
            address == hardware::kGpuStatus) {
            services_.write_gpu_register(address, value);
        }
    }

private:
    PlatformServices& services_;
};

}  // namespace r6
