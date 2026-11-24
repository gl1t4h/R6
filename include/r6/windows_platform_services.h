#pragma once

#include <filesystem>
#include <fstream>

#include "r6/platform_services.h"

namespace r6 {

class WindowsPlatformServices final : public PlatformServices {
public:
    explicit WindowsPlatformServices(const std::filesystem::path& raw_bin_path);

    std::uint16_t read_controller(std::size_t port) override;
    void submit_audio_samples(const std::int16_t* samples, std::size_t count) override;
    void present_frame(const std::uint32_t* pixels, std::size_t count) override;
    std::uint32_t read_timer(std::size_t timer) override;
    void write_gpu_register(std::uint32_t register_address, std::uint32_t value) override;
    bool read_disc_sector(
        std::uint32_t raw_sector,
        std::uint8_t* destination,
        std::size_t destination_size) override;
    bool bios_call(std::uint32_t vector, CpuState& cpu) override;

private:
    std::ifstream disc_;
};

}  // namespace r6
