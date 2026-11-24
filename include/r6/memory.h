#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace r6 {

class Memory {
public:
    explicit Memory(std::size_t bytes) : bytes_(bytes, 0) {}

    std::uint8_t read8(std::uint32_t address) const {
        return bytes_.at(index(address));
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

private:
    std::size_t index(std::uint32_t address) const {
        if (address >= bytes_.size()) {
            throw std::out_of_range("R6 memory address outside local memory");
        }
        return static_cast<std::size_t>(address);
    }

    std::vector<std::uint8_t> bytes_;
};

}  // namespace r6
