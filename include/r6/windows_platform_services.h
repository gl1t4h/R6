#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "r6/bios_services.h"
#include "r6/platform_services.h"
#include "r6/software_gpu.h"

namespace r6 {

class Memory;

class WindowsPlatformServices final : public PlatformServices {
public:
    explicit WindowsPlatformServices(const std::filesystem::path& raw_bin_path);

    std::uint16_t read_controller(std::size_t port) override;
    void submit_audio_samples(const std::int16_t* samples, std::size_t count) override;
    void present_frame(const std::uint32_t* pixels, std::size_t count) override;
    std::uint32_t read_timer(std::size_t timer) override;
    void write_gpu_register(std::uint32_t register_address, std::uint32_t value) override;
    std::uint32_t read_gpu_status() override;
    std::uint32_t read_gpu_data() override;
    bool read_disc_sector(
        std::uint32_t raw_sector,
        std::uint8_t* destination,
        std::size_t destination_size) override;
    bool bios_call(std::uint32_t pc, CpuState& cpu, Memory& memory) override;
    std::uint32_t interrupt_entry(std::uint32_t priority) override;
    std::uint32_t hooked_entry() override;

    void set_controller_state(std::size_t port, std::uint16_t active_low_state);
    const std::vector<std::uint32_t>& frame_buffer() const { return frame_; }
    const std::vector<std::int16_t>& audio_buffer() const { return audio_; }
    const SoftwareGpu& gpu() const { return gpu_; }
    const BiosServices& bios() const { return bios_; }
    std::uint32_t last_bios_pc() const { return last_bios_pc_; }

private:
    std::ifstream disc_;
    std::uint16_t controller_state_[2]{0xFFFF, 0xFFFF};
    bool controller_override_[2]{false, false};
    std::vector<std::uint32_t> frame_;
    std::vector<std::int16_t> audio_;
    SoftwareGpu gpu_;
    BiosServices bios_;
    std::uint32_t last_bios_pc_{};
};

}  // namespace r6
