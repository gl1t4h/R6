#include "r6/windows_platform_services.h"
#include "r6/hardware_map.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace r6 {

namespace {
constexpr std::uint64_t kRawSectorSize = 2352;
constexpr std::uint64_t kUserDataOffset = 24;
constexpr std::uint64_t kUserDataSize = 2048;

bool has_chd_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension == ".chd";
}
}  // namespace

WindowsPlatformServices::WindowsPlatformServices(
    const std::filesystem::path& image_path) {
    // The kernel's GPU vectors write the same registers the interpreter does,
    // so route them at the modelled device instead of duplicating it.
    bios_.set_gpu_access(
        [this](std::uint32_t register_address, std::uint32_t value) {
            write_gpu_register(register_address, value);
        },
        [this] { return gpu_.status(); });

    // A CHD is a container, not a byte-addressable BIN, so sector reads take
    // the decoder path instead of seeking in the file.
    if (has_chd_extension(image_path)) {
        auto image = std::make_unique<ChdImage>();
        std::string error;
        if (image->open(image_path, error)) {
            chd_ = std::move(image);
            disc_ready_ = true;
        } else {
            disc_error_ = error;
        }
        return;
    }

    disc_.open(image_path, std::ios::binary);
    disc_ready_ = static_cast<bool>(disc_);
    if (!disc_ready_) disc_error_ = "cannot open " + image_path.string();
}

std::uint16_t WindowsPlatformServices::read_controller(std::size_t port) {
    if (port >= 2) return 0xFFFF;
    if (controller_override_[port]) return controller_state_[port];

#ifdef _WIN32
    auto pressed = [](int key) {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    };
    std::uint16_t state = 0xFFFF;
    if (pressed(VK_RETURN)) state &= static_cast<std::uint16_t>(~(1u << 3));
    if (pressed(VK_SHIFT)) state &= static_cast<std::uint16_t>(~(1u << 0));
    if (pressed(VK_UP)) state &= static_cast<std::uint16_t>(~(1u << 4));
    if (pressed(VK_RIGHT)) state &= static_cast<std::uint16_t>(~(1u << 5));
    if (pressed(VK_DOWN)) state &= static_cast<std::uint16_t>(~(1u << 6));
    if (pressed(VK_LEFT)) state &= static_cast<std::uint16_t>(~(1u << 7));
    if (pressed('Z')) state &= static_cast<std::uint16_t>(~(1u << 14));
    if (pressed('X')) state &= static_cast<std::uint16_t>(~(1u << 13));
    if (pressed('A')) state &= static_cast<std::uint16_t>(~(1u << 12));
    if (pressed('S')) state &= static_cast<std::uint16_t>(~(1u << 15));
    return state;
#else
    return 0xFFFF;
#endif
}

void WindowsPlatformServices::submit_audio_samples(
    const std::int16_t* samples,
    std::size_t count) {
    if (samples == nullptr) return;
    audio_.insert(audio_.end(), samples, samples + count);
    constexpr std::size_t kMaxBufferedSamples = 44100 * 2;
    if (audio_.size() > kMaxBufferedSamples) {
        audio_.erase(audio_.begin(), audio_.begin() +
            static_cast<std::ptrdiff_t>(audio_.size() - kMaxBufferedSamples));
    }
}

void WindowsPlatformServices::present_frame(
    const std::uint32_t* pixels,
    std::size_t count) {
    if (pixels == nullptr) return;
    frame_.assign(pixels, pixels + count);
}

std::uint32_t WindowsPlatformServices::read_timer(std::size_t) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void WindowsPlatformServices::write_gpu_register(
    std::uint32_t register_address,
    std::uint32_t value) {
    if (register_address == hardware::kGpuCommand) {
        gpu_.submit_gp0(value);
        return;
    }
    if (register_address == hardware::kGpuStatus) {
        gpu_.submit_gp1(value);
    }
}

std::uint32_t WindowsPlatformServices::read_gpu_status() {
    return gpu_.status();
}

std::uint32_t WindowsPlatformServices::read_gpu_data() {
    return gpu_.read_gp0_data();
}

bool WindowsPlatformServices::read_disc_sector(
    std::uint32_t raw_sector,
    std::uint8_t* destination,
    std::size_t destination_size) {
    if (chd_) {
        if (chd_->read_user_sector(raw_sector, destination, destination_size)) {
            return true;
        }
        disc_error_ = chd_->error();
        return false;
    }
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

bool WindowsPlatformServices::read_disc_header(
    std::uint32_t raw_sector,
    std::uint8_t* destination,
    std::size_t destination_size) {
    if (chd_) {
        if (chd_->read_sector_header(raw_sector, destination,
                                     destination_size)) {
            return true;
        }
        disc_error_ = chd_->error();
        return false;
    }
    if (!disc_ || destination == nullptr || destination_size < 12) return false;
    // A raw Mode 2 sector is a 12-byte sync pattern, then the 4-byte address
    // and mode, then the 8-byte subheader: the drive presents the last 12
    // bytes to the guest.
    const auto offset =
        static_cast<std::uint64_t>(raw_sector) * kRawSectorSize + 12;
    disc_.clear();
    disc_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    disc_.read(reinterpret_cast<char*>(destination), 12);
    const bool complete = disc_.gcount() == 12;
    disc_.clear();
    return complete;
}

bool WindowsPlatformServices::bios_call(std::uint32_t pc,
                                        CpuState& cpu,
                                        Memory& memory) {
    BiosTable table{};
    if (!BiosServices::trap_table(pc, table)) return false;
    last_bios_pc_ = pc;
    bios_.dispatch(table, cpu, memory);
    return true;
}

std::uint32_t WindowsPlatformServices::interrupt_entry(std::uint32_t priority) {
    return bios_.interrupt_entry(priority);
}

std::uint32_t WindowsPlatformServices::entry_int() {
    return bios_.entry_int();
}

bool WindowsPlatformServices::take_return_from_exception() {
    return bios_.take_return_from_exception();
}

void WindowsPlatformServices::set_controller_state(
    std::size_t port,
    std::uint16_t active_low_state) {
    if (port >= 2) return;
    controller_state_[port] = active_low_state;
    controller_override_[port] = true;
}

}  // namespace r6
