#include "r6/windows_platform_services.h"

#include <chrono>
#include <cstdint>

namespace r6 {

namespace {
constexpr std::uint64_t kRawSectorSize = 2352;
constexpr std::uint64_t kUserDataOffset = 24;
constexpr std::uint64_t kUserDataSize = 2048;
}

WindowsPlatformServices::WindowsPlatformServices(
    const std::filesystem::path& raw_bin_path)
    : disc_(raw_bin_path, std::ios::binary) {}

std::uint16_t WindowsPlatformServices::read_controller(std::size_t) {
    return 0;
}

void WindowsPlatformServices::submit_audio_samples(
    const std::int16_t*,
    std::size_t) {}

void WindowsPlatformServices::present_frame(
    const std::uint32_t*,
    std::size_t) {}

std::uint32_t WindowsPlatformServices::read_timer(std::size_t) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void WindowsPlatformServices::write_gpu_register(
    std::uint32_t,
    std::uint32_t) {}

bool WindowsPlatformServices::read_disc_sector(
    std::uint32_t raw_sector,
    std::uint8_t* destination,
    std::size_t destination_size) {
    if (!disc_ || destination == nullptr || destination_size < kUserDataSize) {
        return false;
    }
    const auto offset =
        static_cast<std::uint64_t>(raw_sector) * kRawSectorSize + kUserDataOffset;
    disc_.clear();
    disc_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    disc_.read(
        reinterpret_cast<char*>(destination),
        static_cast<std::streamsize>(kUserDataSize));
    return disc_.gcount() == static_cast<std::streamsize>(kUserDataSize);
}

bool WindowsPlatformServices::bios_call(std::uint32_t, CpuState&) {
    return false;
}

}  // namespace r6
