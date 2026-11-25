#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace r6 {

class Memory {
public:
    // translate_kseg maps the 0x80000000/0xA0000000 kernel aliases into the
    // buffer, which requires the buffer size to be a power of two.
    explicit Memory(std::size_t bytes, bool translate_kseg = false)
        : bytes_(bytes, 0), translate_kseg_(translate_kseg) {}

    std::uint8_t read8(std::uint32_t address) const {
        return bytes_.at(index(address));
    }

    // Returns false instead of throwing when the address is unmapped, so
    // callers that walk guest strings can stop cleanly at the boundary.
    bool try_read8(std::uint32_t address, std::uint8_t& value) const noexcept {
        const auto mapped = translate(address);
        if (mapped >= bytes_.size()) return false;
        value = bytes_[mapped];
        return true;
    }

    std::uint16_t read16(std::uint32_t address) const {
        return static_cast<std::uint16_t>(read8(address)) |
               (static_cast<std::uint16_t>(read8(address + 1)) << 8);
    }

    std::uint32_t read32(std::uint32_t address) const {
        return static_cast<std::uint32_t>(read16(address)) |
               (static_cast<std::uint32_t>(read16(address + 2)) << 16);
    }

    void write8(std::uint32_t address, std::uint8_t value) {
        bytes_.at(index(address)) = value;
    }

    void write16(std::uint32_t address, std::uint16_t value) {
        write8(address, static_cast<std::uint8_t>(value));
        write8(address + 1, static_cast<std::uint8_t>(value >> 8));
    }

    void write32(std::uint32_t address, std::uint32_t value) {
        write16(address, static_cast<std::uint16_t>(value));
        write16(address + 2, static_cast<std::uint16_t>(value >> 16));
    }

    std::size_t size() const noexcept { return bytes_.size(); }

    // Raw copy support for the differential harness: checkpoints compare all
    // of RAM without routing through the hardware map.
    std::vector<std::uint8_t> snapshot() const { return bytes_; }

    void restore(const std::vector<std::uint8_t>& bytes) {
        if (bytes.size() != bytes_.size()) {
            throw std::invalid_argument("R6 memory snapshot size mismatch");
        }
        bytes_ = bytes;
    }

private:
    std::size_t translate(std::uint32_t address) const noexcept {
        if (translate_kseg_ && address >= 0x80000000u) {
            return static_cast<std::size_t>(address &
                                            (static_cast<std::uint32_t>(
                                                 bytes_.size()) - 1u));
        }
        return static_cast<std::size_t>(address);
    }

    std::size_t index(std::uint32_t address) const {
        const auto mapped = translate(address);
        if (mapped >= bytes_.size()) {
            throw std::out_of_range("R6 memory address outside local memory");
        }
        return mapped;
    }

    std::vector<std::uint8_t> bytes_;
    bool translate_kseg_{false};
};

}  // namespace r6
