// r6-run: development runner for the user's own PlayStation 1 copy.
//
// It loads the game executable out of a local BIN/CUE image, executes it on the
// native R3000A interpreter, and reports what the program did through the
// native platform services. It is a pipeline reference, not a playable build:
// GTE geometry, DMA, interrupts and the CD-ROM controller are not implemented
// yet, so the game cannot reach its render loop.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "r6/mips_interpreter.h"
#include "r6/software_gpu.h"
#include "r6/windows_platform_services.h"

namespace {

constexpr std::uint32_t kSectorSize = 2048;
constexpr std::uint32_t kRamSize = 2u * 1024u * 1024u;
constexpr std::uint32_t kRamMask = kRamSize - 1u;

struct DiscFile {
    std::uint32_t lba{0};
    std::uint32_t size{0};
    std::string name;
};

std::uint32_t le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string upper(std::string text) {
    for (auto& character : text) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return text;
}

std::vector<std::uint8_t> read_sectors(r6::WindowsPlatformServices& disc,
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

std::vector<DiscFile> list_root(r6::WindowsPlatformServices& disc) {
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
            if (name_length == 0 || offset + 33 + name_length > directory.size()) {
                break;
            }
            std::string name(
                reinterpret_cast<const char*>(record + 33), name_length);
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

std::vector<std::uint8_t> read_file(r6::WindowsPlatformServices& disc,
                                    const DiscFile& file) {
    const auto sectors = (file.size + kSectorSize - 1) / kSectorSize;
    auto data = read_sectors(disc, file.lba, sectors);
    if (data.size() > file.size) data.resize(file.size);
    return data;
}

std::string boot_name(const std::vector<std::uint8_t>& system_cnf) {
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

bool parse_exe(const std::vector<std::uint8_t>& data, ExeImage& image) {
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
                         data.begin() + 0x800 + static_cast<std::ptrdiff_t>(copy));
    return image.entry != 0;
}

std::filesystem::path resolve_bin(const std::filesystem::path& input) {
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

void write_ppm(const std::vector<std::uint32_t>& frame,
               std::uint32_t width,
               std::uint32_t height,
               const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    out << "P6\n" << width << " " << height << "\n255\n";
    for (const auto pixel : frame) {
        const char rgb[3] = {
            static_cast<char>((pixel >> 16) & 0xFFu),
            static_cast<char>((pixel >> 8) & 0xFFu),
            static_cast<char>(pixel & 0xFFu)};
        out.write(rgb, 3);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path input;
    std::uint64_t steps = 5000000;
    std::filesystem::path frame_path;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--steps" && index + 1 < argc) {
            steps = std::stoull(argv[++index]);
        } else if (argument == "--dump-frame" && index + 1 < argc) {
            frame_path = argv[++index];
        } else if (input.empty()) {
            input = argument;
        }
    }

    if (input.empty()) {
        std::cerr << "usage: r6-run <game.cue|game.bin> [--steps N]"
                     " [--dump-frame frame.ppm]\n"
                  << "Runs your own local copy. No game data is copied or"
                     " uploaded.\n";
        return 2;
    }

    const auto bin_path = resolve_bin(input);
    std::error_code error;
    if (!std::filesystem::exists(bin_path, error)) {
        std::cerr << "r6-run: disc image not found: " << bin_path << "\n";
        return 2;
    }

    r6::WindowsPlatformServices services(bin_path);
    r6::Memory ram(kRamSize, true);

    const auto files = list_root(services);
    if (files.empty()) {
        std::cerr << "r6-run: no ISO9660 volume found in " << bin_path << "\n";
        return 1;
    }

    std::string exe_name;
    for (const auto& file : files) {
        if (upper(file.name) == "SYSTEM.CNF") {
            exe_name = boot_name(read_file(services, file));
        }
    }
    if (exe_name.empty()) {
        for (const auto& file : files) {
            if (upper(file.name).find("SLUS") != std::string::npos) {
                exe_name = file.name;
                break;
            }
        }
    }
    if (exe_name.empty()) {
        std::cerr << "r6-run: could not determine the boot executable\n";
        return 1;
    }

    const DiscFile* executable = nullptr;
    for (const auto& file : files) {
        if (upper(file.name) == upper(exe_name)) {
            executable = &file;
            break;
        }
    }
    if (executable == nullptr) {
        std::cerr << "r6-run: " << exe_name << " is not present in the image\n";
        return 1;
    }

    ExeImage image;
    if (!parse_exe(read_file(services, *executable), image)) {
        std::cerr << "r6-run: " << exe_name << " has no valid PS-X EXE header\n";
        return 1;
    }

    for (std::size_t index = 0; index < image.payload.size(); ++index) {
        ram.write8((image.load_address + static_cast<std::uint32_t>(index)) &
                       kRamMask,
                   image.payload[index]);
    }

    r6::MipsInterpreter interpreter(ram, services);
    auto& cpu = interpreter.cpu();
    cpu.pc = image.entry;
    cpu.gpr[28] = image.gp;
    cpu.gpr[29] =
        image.stack_base != 0 ? image.stack_base : 0x801FFFF0u;
    cpu.gpr[31] = 0;

    std::cout << "R6 development runner\n"
              << "This executes your local copy; it is not a playable game.\n\n"
              << "Disc: " << bin_path.filename().string() << "\n"
              << "EXE:  " << executable->name << " (" << image.size
              << " bytes at LBA " << executable->lba << ")\n"
              << "Entry 0x" << std::hex << image.entry << std::dec
              << ", GP 0x" << std::hex << image.gp << std::dec
              << ", SP 0x" << std::hex << cpu.gpr[29] << std::dec << "\n"
              << "Running " << steps << " instructions...\n";

    const auto result = interpreter.run(steps);

    {
        std::cout << "\nBIOS calls: " << services.bios().total_calls();
        if (services.bios().exited()) {
            std::cout << " (guest called _exit with code "
                      << services.bios().exit_code() << ")";
        }
        std::cout << "\n";
        for (const auto& entry : services.bios().calls()) {
            const auto table =
                static_cast<r6::BiosTable>(entry.first.first);
            const char name = table == r6::BiosTable::a0
                                  ? 'A'
                                  : (table == r6::BiosTable::b0 ? 'B' : 'C');
            std::cout << "  " << name << "(0x" << std::hex << entry.first.second
                      << std::dec << ") x" << entry.second << "\n";
        }
    }

    std::cout << "\nExecuted " << result.steps << " instructions\n"
              << "Stopped at PC 0x" << std::hex << result.pc << std::dec;
    switch (result.reason) {
        case r6::StopReason::step_budget:
            std::cout << " (step budget reached)\n";
            break;
        case r6::StopReason::returned:
            std::cout << " (returned to address zero)\n";
            break;
        case r6::StopReason::unsupported_instruction:
            std::cout << " (unsupported instruction 0x" << std::hex
                      << result.instruction << std::dec << ")\n";
            break;
        case r6::StopReason::bios_exit:
            std::cout << " (BIOS exit)\n";
            break;
    }

    const auto& gpu = services.gpu();
    std::cout << "GPU: " << gpu.rendered_primitives()
              << " primitives, " << gpu.unsupported_commands()
              << " unsupported commands\n"
              << "GTE: " << interpreter.gte_commands()
              << " commands (not computed yet)\n"
              << "DMA: " << interpreter.dma_writes()
              << " register writes (not executed yet)\n"
              << "BIOS: " << services.bios().unhandled_call_count()
              << " unhandled vectors\n";

    if (!services.bios().tty_output().empty()) {
        std::cout << "\nGuest TTY output:\n" << services.bios().tty_output()
                  << "\n";
    }

    std::cout << "\nLast PCs:";
    for (const auto pc : interpreter.recent_pcs()) {
        std::cout << " 0x" << std::hex << pc << std::dec;
    }
    std::cout << "\n";

    if (!frame_path.empty()) {
        write_ppm(gpu.display_frame(), r6::SoftwareGpu::kDisplayWidth,
                  r6::SoftwareGpu::kDisplayHeight, frame_path);
        std::cout << "Wrote frame to " << frame_path.string() << "\n";
    }

    return result.reason == r6::StopReason::unsupported_instruction ? 1 : 0;
}
