#pragma once

#include <cstddef>
#include <cstdint>

namespace r6 {

struct CpuState;
class Memory;

class PlatformServices {
public:
    virtual ~PlatformServices() = default;

    virtual std::uint16_t read_controller(std::size_t port) = 0;
    virtual void submit_audio_samples(const std::int16_t* samples, std::size_t count) = 0;
    virtual void present_frame(const std::uint32_t* pixels, std::size_t count) = 0;
    virtual std::uint32_t read_timer(std::size_t timer) = 0;
    virtual void write_gpu_register(
        std::uint32_t register_address,
        std::uint32_t value) = 0;
    virtual std::uint32_t read_gpu_status() = 0;
    virtual std::uint32_t read_gpu_data() = 0;
    virtual bool read_disc_sector(
        std::uint32_t lba,
        std::uint8_t* destination,
        std::size_t destination_size) = 0;
    // pc identifies the 0xA0/0xB0/0xC0 trap being serviced. Returns false when
    // the address is not a kernel entry point.
    virtual bool bios_call(std::uint32_t pc, CpuState& cpu, Memory& memory) = 0;
};

class NullPlatformServices final : public PlatformServices {
public:
    std::uint16_t read_controller(std::size_t) override { return 0; }
    void submit_audio_samples(const std::int16_t*, std::size_t) override {}
    void present_frame(const std::uint32_t*, std::size_t) override {}
    std::uint32_t read_timer(std::size_t) override { return 0; }
    void write_gpu_register(std::uint32_t, std::uint32_t) override {}
    std::uint32_t read_gpu_status() override { return 0; }
    std::uint32_t read_gpu_data() override { return 0; }
    bool read_disc_sector(std::uint32_t, std::uint8_t*, std::size_t) override {
        return false;
    }
    bool bios_call(std::uint32_t, CpuState&, Memory&) override { return false; }
};

}  // namespace r6
