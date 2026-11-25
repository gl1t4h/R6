#pragma once

// Shared disc bootstrapping: locate a BIN/CUE, read the ISO9660 root, follow
// SYSTEM.CNF and parse the PS-X EXE header. Used by both the console runner and
// the windowed player so the two cannot drift apart.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "r6/windows_platform_services.h"

namespace r6::boot {

constexpr std::uint32_t kSectorSize = 2048;

struct DiscFile {
    std::uint32_t lba{0};
    std::uint32_t size{0};
    std::string name;
};

inline std::uint32_t le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::string upper(std::string text) {
    for (auto& character : text) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return text;
}

inline std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension;
}

// The loader reads raw BIN/CUE only. A .chd is a compressed container, so the
// useful thing to report is how to expand it, not that the file is missing.
// Both front ends print the same text from here so the two cannot drift.
inline bool is_chd_image(const std::filesystem::path& path) {
    return lowercase_extension(path) == ".chd";
}

inline const char* chd_hint() {
    return "A .chd image is a compressed container; the loader reads raw "
           "BIN/CUE only.\n"
           "Expand it first with chdman (shipped with MAME), keeping the "
           "output outside this repository:\n"
           "  chdman extractcd -i game.chd -o game.cue -ob game.bin\n"
           "See the \"Disc formats\" section in README.md.";
}

inline std::vector<std::uint8_t> read_sectors(WindowsPlatformServices& disc,
                                              std::uint32_t lba,
                                              std::uint32_t count) {
    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(count) * kSectorSize, 0);
    for (std::uint32_t index = 0; index < count; ++index) {
        auto* destination =
            data.data() + static_cast<std::size_t>(index) * kSectorSize;
        if (!disc.read_disc_sector(lba + index, destination, kSectorSize)) {
            data.resize(static_cast<std::size_t>(index) * kSectorSize);
            break;
        }
    }
    return data;
}

inline std::vector<DiscFile> list_root(WindowsPlatformServices& disc) {
    std::vector<DiscFile> files;
    const auto descriptor = read_sectors(disc, 16, 1);
    if (descriptor.size() < kSectorSize || descriptor[0] != 1 ||
        std::memcmp(descriptor.data() + 1, "CD001", 5) != 0) {
        return files;
    }

    const auto* root_record = descriptor.data() + 156;
    const auto root_lba = le32(root_record + 2);
    const auto root_size = le32(root_record + 10);
    const auto sectors = (root_size + kSectorSize - 1) / kSectorSize;
    const auto directory = read_sectors(disc, root_lba, sectors);

    for (std::uint32_t sector = 0; sector < sectors; ++sector) {
        std::size_t offset = static_cast<std::size_t>(sector) * kSectorSize;
        const auto end = offset + kSectorSize;
        while (offset + 33 <= directory.size() && offset < end) {
            const auto length = directory[offset];
            if (length == 0) break;
            const auto* record = directory.data() + offset;
            const auto name_length = record[32];
            if (name_length == 0 ||
                offset + 33 + name_length > directory.size()) {
                break;
            }
            std::string name(reinterpret_cast<const char*>(record + 33),
                             name_length);
            const auto separator = name.find(';');
            if (separator != std::string::npos) name.resize(separator);
            const auto flags = record[25];
            if ((flags & 0x02u) == 0) {
                files.push_back({le32(record + 2), le32(record + 10), name});
            }
            offset += length;
        }
    }
    return files;
}

inline std::vector<std::uint8_t> read_file(WindowsPlatformServices& disc,
                                           const DiscFile& file) {
    const auto sectors = (file.size + kSectorSize - 1) / kSectorSize;
    auto data = read_sectors(disc, file.lba, sectors);
    if (data.size() > file.size) data.resize(file.size);
    return data;
}

inline std::string boot_name(const std::vector<std::uint8_t>& system_cnf) {
    const std::string text(system_cnf.begin(), system_cnf.end());
    const auto boot = text.find("BOOT");
    if (boot == std::string::npos) return {};
    auto line = text.find('=', boot);
    if (line == std::string::npos) return {};
    ++line;
    while (line < text.size() && (text[line] == ' ' || text[line] == '\t')) {
        ++line;
    }
    auto end = text.find_first_of("\r\n", line);
    if (end == std::string::npos) end = text.size();
    auto name = text.substr(line, end - line);
    const auto colon = name.rfind(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    while (!name.empty() && (name.front() == '\\' || name.front() == '/')) {
        name.erase(name.begin());
    }
    const auto separator = name.find(';');
    if (separator != std::string::npos) name.resize(separator);
    return name;
}

struct ExeImage {
    std::uint32_t entry{0};
    std::uint32_t gp{0};
    std::uint32_t load_address{0};
    std::uint32_t size{0};
    std::uint32_t stack_base{0};
    std::uint32_t stack_size{0};
    std::vector<std::uint8_t> payload;
};

inline bool parse_exe(const std::vector<std::uint8_t>& data, ExeImage& image) {
    if (data.size() < 0x800 || std::memcmp(data.data(), "PS-X EXE", 8) != 0) {
        return false;
    }
    image.entry = le32(data.data() + 0x10);
    image.gp = le32(data.data() + 0x14);
    image.load_address = le32(data.data() + 0x18);
    image.size = le32(data.data() + 0x1C);
    image.stack_base = le32(data.data() + 0x30);
    image.stack_size = le32(data.data() + 0x34);

    const auto available = data.size() - 0x800;
    const auto copy = std::min<std::size_t>(image.size, available);
    image.payload.assign(data.begin() + 0x800,
                         data.begin() + 0x800 +
                             static_cast<std::ptrdiff_t>(copy));
    return image.entry != 0;
}

inline std::filesystem::path resolve_bin(const std::filesystem::path& input) {
    auto extension = input.extension().string();
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (extension != ".cue") return input;

    std::ifstream cue(input);
    std::string line;
    while (std::getline(cue, line)) {
        const auto file_keyword = line.find("FILE");
        if (file_keyword == std::string::npos) continue;
        const auto first_quote = line.find('"', file_keyword);
        if (first_quote == std::string::npos) continue;
        const auto second_quote = line.find('"', first_quote + 1);
        if (second_quote == std::string::npos) continue;
        return input.parent_path() /
               line.substr(first_quote + 1, second_quote - first_quote - 1);
    }
    return input;
}

inline void write_ppm(const std::vector<std::uint32_t>& frame,
                      std::uint32_t width,
                      std::uint32_t height,
                      const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    out << "P6\n" << width << " " << height << "\n255\n";
    for (const auto pixel : frame) {
        const char rgb[3] = {static_cast<char>((pixel >> 16) & 0xFFu),
                             static_cast<char>((pixel >> 8) & 0xFFu),
                             static_cast<char>(pixel & 0xFFu)};
        out.write(rgb, 3);
    }
}

}  // namespace r6::boot
