#pragma once

// Local reference snapshot: CPU registers plus all 2 MiB of main RAM.
//
// The snapshot is derived from the user's own disc and stays outside the
// repository. It exists so the translator and the native runner can start from
// the exact state the interpreter reached, including code the boot loader
// streamed into RAM.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "r6/cpu_state.h"
#include "r6/memory.h"

namespace r6 {

constexpr std::uint32_t kSnapshotVersion = 1;
constexpr char kSnapshotMagic[4] = {'R', '6', 'S', '1'};

inline void write_u32(std::ostream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8) & 0xFFu),
        static_cast<char>((value >> 16) & 0xFFu),
        static_cast<char>((value >> 24) & 0xFFu),
    };
    out.write(bytes, 4);
}

inline bool read_u32(std::istream& in, std::uint32_t& value) {
    unsigned char bytes[4];
    if (!in.read(reinterpret_cast<char*>(bytes), 4)) return false;
    value = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

inline bool write_snapshot(const std::filesystem::path& path,
                           const CpuState& cpu,
                           const Memory& memory) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out.write(kSnapshotMagic, 4);
    write_u32(out, kSnapshotVersion);
    for (const auto value : cpu.gpr) write_u32(out, value);
    write_u32(out, cpu.hi);
    write_u32(out, cpu.lo);
    write_u32(out, cpu.pc);

    const auto ram = memory.snapshot();
    write_u32(out, static_cast<std::uint32_t>(ram.size()));
    out.write(reinterpret_cast<const char*>(ram.data()),
              static_cast<std::streamsize>(ram.size()));
    return static_cast<bool>(out);
}

inline bool read_snapshot(const std::filesystem::path& path,
                          CpuState& cpu,
                          Memory& memory) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    char magic[4];
    if (!in.read(magic, 4) || std::memcmp(magic, kSnapshotMagic, 4) != 0) {
        return false;
    }
    std::uint32_t version = 0;
    if (!read_u32(in, version) || version != kSnapshotVersion) return false;
    for (auto& value : cpu.gpr) {
        if (!read_u32(in, value)) return false;
    }
    if (!read_u32(in, cpu.hi) || !read_u32(in, cpu.lo) ||
        !read_u32(in, cpu.pc)) {
        return false;
    }

    std::uint32_t ram_size = 0;
    if (!read_u32(in, ram_size) || ram_size != memory.size()) return false;
    std::vector<std::uint8_t> ram(ram_size);
    if (!in.read(reinterpret_cast<char*>(ram.data()),
                 static_cast<std::streamsize>(ram.size()))) {
        return false;
    }
    memory.restore(ram);
    return true;
}

}  // namespace r6
