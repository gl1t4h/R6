// r6-run: development runner for the user's own PlayStation 1 copy.
//
// It loads the game executable out of a local BIN/CUE image, executes it on the
// native R3000A interpreter, and reports what the program did through the
// native platform services. It is a pipeline reference, not a playable build:
// GTE geometry and the CD-ROM streaming path are still incomplete, so the game
// cannot reach its render loop.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "r6/boot_image.h"
#include "r6/chd_image.h"
#include "r6/mips_interpreter.h"
#include "r6/software_gpu.h"
#include "r6/windows_platform_services.h"

namespace {

// Disc plumbing and PS-X EXE parsing live in r6/boot_image.h so the windowed
// player and this runner share one implementation.
using r6::boot::boot_name;
using r6::boot::DiscFile;
using r6::boot::ExeImage;
using r6::boot::list_root;
using r6::boot::parse_exe;
using r6::boot::read_file;
using r6::boot::resolve_bin;
using r6::boot::upper;
using r6::boot::write_ppm;

constexpr std::uint32_t kRamSize = 2u * 1024u * 1024u;
constexpr std::uint32_t kRamMask = kRamSize - 1u;
constexpr std::uint32_t kDefaultStack = 0x801FFFF0u;
// This title's C runtime reads a stack limit from .data and computes a stack
// outside RAM. The retail loader supplies a usable one, so correct the global
// before the guest's startup code reads it, as the windowed player does.
constexpr std::uint32_t kCrStackGlobal = 0x800B3E70u;

void put_le32(std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

// Writes the executable back out in the PS-X EXE format the local translation
// generator consumes. The file is derived from the user's own disc and stays
// outside the repository.
bool write_exe(const std::filesystem::path& path, const ExeImage& image) {
    std::vector<std::uint8_t> header(0x800, 0);
    std::memcpy(header.data(), "PS-X EXE", 8);
    put_le32(header, 0x10, image.entry);
    put_le32(header, 0x14, image.gp);
    put_le32(header, 0x18, image.load_address);
    put_le32(header, 0x1C, static_cast<std::uint32_t>(image.payload.size()));
    put_le32(header, 0x30, image.stack_base);
    put_le32(header, 0x34, image.stack_size);

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(image.payload.data()),
              static_cast<std::streamsize>(image.payload.size()));
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path input;
    std::uint64_t steps = 5000000;
    std::filesystem::path frame_path;
    std::uint32_t entry_override = 0;
    std::filesystem::path extract_exe_path;
    bool deliver_vblank = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pokes;
    std::vector<std::uint32_t> peeks;
    std::vector<std::uint32_t> watched_pcs;
    std::vector<std::uint32_t> watched_args;
    bool hot_enabled = false;
    std::uint32_t trace_reg = 0;
    std::uint32_t find_value = 0;
    std::uint32_t search_imm = 0;
    bool search_code = false;
    bool trace_cd = false;
    bool dump_stream = false;
    // Guest ranges to print after the run, for reading a hot loop's code.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dump_ranges;
    // Addresses holding a code pointer, dumped after the run: ADDR:COUNT.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dump_pointers;
    bool chd_info = false;
    std::vector<std::uint32_t> watch_writes;
    std::string search_text;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--steps" && index + 1 < argc) {
            steps = std::stoull(argv[++index]);
        } else if (argument == "--dump-frame" && index + 1 < argc) {
            frame_path = argv[++index];
        } else if (argument == "--entry" && index + 1 < argc) {
            entry_override = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0));
        } else if (argument == "--extract-exe" && index + 1 < argc) {
            extract_exe_path = argv[++index];
        } else if (argument == "--vblank") {
            deliver_vblank = true;
        } else if (argument == "--trace-reg" && index + 1 < argc) {
            trace_reg = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0));
        } else if (argument == "--find-value" && index + 1 < argc) {
            find_value = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0));
        } else if (argument == "--search-code" && index + 1 < argc) {
            search_imm = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0));
            search_code = true;
        } else if (argument == "--search-string" && index + 1 < argc) {
            search_text = argv[++index];
        } else if (argument == "--trace-cd") {
            trace_cd = true;
        } else if (argument == "--dump-range" && index + 1 < argc) {
            // ADDR:COUNT, both base 0, e.g. 0x80015400:96.
            const std::string spec = argv[++index];
            const auto split = spec.find(':');
            if (split == std::string::npos) {
                std::cerr << "r6-run: --dump-range expects ADDR:COUNT\n";
                return 2;
            }
            dump_ranges.emplace_back(
                static_cast<std::uint32_t>(
                    std::stoul(spec.substr(0, split), nullptr, 0)),
                static_cast<std::uint32_t>(
                    std::stoul(spec.substr(split + 1), nullptr, 0)));
        } else if (argument == "--dump-at-pointer" && index + 1 < argc) {
            // ADDR:COUNT, where ADDR holds the code address to dump.
            const std::string spec = argv[++index];
            const auto split = spec.find(':');
            if (split == std::string::npos) {
                std::cerr << "r6-run: --dump-at-pointer expects ADDR:COUNT\n";
                return 2;
            }
            dump_pointers.emplace_back(
                static_cast<std::uint32_t>(
                    std::stoul(spec.substr(0, split), nullptr, 0)),
                static_cast<std::uint32_t>(
                    std::stoul(spec.substr(split + 1), nullptr, 0)));
        } else if (argument == "--dump-stream") {
            dump_stream = true;
        } else if (argument == "--chd-info") {
            chd_info = true;
        } else if (argument == "--watch-write" && index + 1 < argc) {
            watch_writes.push_back(static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0)));
        } else if (argument == "--hot") {
            hot_enabled = true;
        } else if (argument == "--watch-args" && index + 1 < argc) {
            watched_args.push_back(static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0)));
        } else if (argument == "--watch-pc" && index + 1 < argc) {
            watched_pcs.push_back(static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0)));
        } else if (argument == "--peek" && index + 1 < argc) {
            peeks.push_back(static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0)));
        } else if (argument == "--poke" && index + 1 < argc) {
            // Debug aid: patch one guest word before the run starts, so a
            // startup global can be corrected without editing the image.
            const std::string poke = argv[++index];
            const auto split = poke.find('=');
            if (split == std::string::npos) {
                std::cerr << "r6-run: --poke expects ADDRESS=VALUE\n";
                return 2;
            }
            pokes.emplace_back(std::stoul(poke.substr(0, split), nullptr, 0),
                               std::stoul(poke.substr(split + 1), nullptr, 0));
        } else if (input.empty()) {
            input = argument;
        }
    }

    if (input.empty()) {
        std::cerr << "usage: r6-run <game.cue|game.bin|game.chd> [--steps N]"
                     " [--entry 0xADDR] [--extract-exe path] [--poke 0xADDR=0xVALUE] [--vblank]"
                     " [--dump-frame frame.ppm] [--dump-range 0xADDR:COUNT]"
                     " [--chd-info]\n"
                  << "Runs your own local copy. No game data is copied or"
                     " uploaded.\n";
        return 2;
    }

    if (chd_info) {
        r6::ChdImage image;
        std::string chd_error;
        if (!image.open(input, chd_error)) {
            std::cerr << "r6-run: " << chd_error << "\n";
            return 1;
        }
        std::cout << image.describe() << "\n";
        return 0;
    }

    const auto bin_path = resolve_bin(input);
    std::error_code error;
    if (!std::filesystem::exists(bin_path, error)) {
        std::cerr << "r6-run: disc image not found: " << bin_path << "\n";
        return 2;
    }

    r6::WindowsPlatformServices services(bin_path);
    if (!services.disc_open()) {
        std::cerr << "r6-run: " << input << ": " << services.disc_error()
                  << "\n";
        return 1;
    }
    r6::Memory ram(kRamSize, true);

    const auto files = list_root(services);
    if (files.empty()) {
        std::cerr << "r6-run: no ISO9660 volume found in " << bin_path << "\n";
        if (!services.disc_error().empty()) {
            std::cerr << "r6-run: " << services.disc_error() << "\n";
        }
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

    if (!extract_exe_path.empty()) {
        if (!write_exe(extract_exe_path, image)) {
            std::cerr << "r6-run: could not write " << extract_exe_path << "\n";
            return 1;
        }
        std::cout << "Wrote " << extract_exe_path << " (entry 0x" << std::hex
                  << image.entry << std::dec << ", " << image.payload.size()
                  << " bytes)\n";
        return 0;
    }

    for (std::size_t index = 0; index < image.payload.size(); ++index) {
        ram.write8((image.load_address + static_cast<std::uint32_t>(index)) &
                       kRamMask,
                   image.payload[index]);
    }

    const auto cr_stack = ram.read32(kCrStackGlobal & kRamMask);
    const bool cr_stack_in_ram =
        cr_stack >= 0x80000000u && cr_stack < 0x80200000u;
    if (!cr_stack_in_ram) {
        ram.write32(kCrStackGlobal & kRamMask, kDefaultStack);
    }

    for (const auto& [address, value] : pokes) {
        ram.write32(address & kRamMask, value);
    }

    r6::MipsInterpreter interpreter(ram, services);
    for (auto address : watched_pcs) interpreter.watch_pc(address);
    for (auto address : watched_args) interpreter.watch_args(address);
    for (auto address : watch_writes) interpreter.watch_write(address);
    interpreter.set_hot_enabled(hot_enabled);
    if (search_code) {
        for (std::uint32_t address = 0x80010000u; address < 0x80200000u;
             address += 4) {
            const auto word = interpreter.peek(address);
            if ((word & 0xFFFFu) != (search_imm & 0xFFFFu)) continue;
            const char* name = nullptr;
            switch (word >> 26) {
                case 0x23: name = "lw"; break;
                case 0x2B: name = "sw"; break;
                case 0x21: name = "lh"; break;
                case 0x29: name = "sh"; break;
                case 0x25: name = "lhu"; break;
                case 0x20: name = "lb"; break;
                case 0x24: name = "lbu"; break;
                case 0x28: name = "sb"; break;
                case 0x09: name = "addiu"; break;
                case 0x0D: name = "ori"; break;
                default: break;
            }
            if (name == nullptr) continue;
            std::cout << "code " << name << " 0x" << std::hex << address
                      << std::dec << " word=0x" << std::hex << word << std::dec
                      << std::endl;
        }
    }
    if (!search_text.empty()) {
        for (std::uint32_t address = 0x80010000u; address < 0x80200000u;
             ++address) {
            bool match = true;
            for (std::size_t offset = 0; offset < search_text.size(); ++offset) {
                if (interpreter.peek(address + offset) == 0 &&
                    ram.read8((address + offset) & kRamMask) != 0) {
                }
                if (ram.read8((address + offset) & kRamMask) !=
                    static_cast<std::uint8_t>(search_text[offset])) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;
            std::cout << "text 0x" << std::hex << address << std::dec
                      << std::endl;
            address += search_text.size() - 1;
        }
    }
    interpreter.set_trace_reg(trace_reg);
    interpreter.set_find_value(find_value);
    auto& cpu = interpreter.cpu();
    cpu.pc = entry_override != 0 ? entry_override : image.entry;
    cpu.gpr[28] = image.gp;
    cpu.gpr[29] = image.stack_base != 0
                      ? image.stack_base
                      : (cr_stack_in_ram ? cr_stack : kDefaultStack);
    cpu.gpr[31] = 0;
    interpreter.set_vblank_enabled(deliver_vblank);

    std::cout << "R6 development runner\n"
              << "This executes your local copy; it is not a playable game.\n\n"
              << "Disc: " << bin_path.filename().string() << "\n"
              << "EXE:  " << executable->name << " (" << image.size
              << " bytes at LBA " << executable->lba << ")\n"
              << "Entry 0x" << std::hex << cpu.pc << std::dec
              << (entry_override != 0 ? " (overridden)" : "")
              << ", GP 0x" << std::hex << image.gp << std::dec
              << ", SP 0x" << std::hex << cpu.gpr[29] << std::dec << "\n";
    // Whether the title brings its own exception handler with it, or installs
    // one during the run, is the first question the vector path raises.
    const auto vector = r6::MipsInterpreter::kExceptionVector & kRamMask;
    std::cout << "Vector 0x" << std::hex
              << r6::MipsInterpreter::kExceptionVector << std::dec << ":";
    for (std::uint32_t word = 0; word < 8; ++word) {
        std::cout << " " << std::hex << std::setw(8) << std::setfill('0')
                  << ram.read32(vector + word * 4);
    }
    std::cout << std::setfill(' ') << std::dec
              << (ram.read32(vector) == 0 ? "  (empty)"
                                          : "  (handler present)")
              << "\n"
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

    for (const auto& [site, value] : interpreter.sr_writes()) {
        std::cout << "SR write at 0x" << std::hex << site << " = 0x" << value
                  << std::dec << "\n";
    }
    if (!interpreter.irq_pcs().empty()) {
        std::cout << "IRQ first PCs:";
        for (auto pc : interpreter.irq_pcs()) {
            std::cout << " 0x" << std::hex << pc;
        }
        std::cout << std::dec << std::endl;
    }
    for (const auto& trace : interpreter.irq_traces()) {
        std::cout << "IRQ trace:";
        for (auto pc : trace) {
            std::cout << " 0x" << std::hex << pc;
        }
        std::cout << std::dec << std::endl;
    }
    for (const auto& trace : interpreter.irq_overrun_traces()) {
        std::cout << "IRQ overrun tail:";
        for (auto pc : trace) {
            std::cout << " 0x" << std::hex << pc;
        }
        std::cout << std::dec << std::endl;
    }
    for (const auto& entry : interpreter.irq_overrun_log()) {
        std::cout << "IRQ " << entry << std::endl;
    }
    for (const auto& hot : interpreter.irq_overrun_hot()) {
        std::cout << "IRQ overrun hot:";
        for (const auto& [pc, count] : hot) {
            std::cout << " 0x" << std::hex << pc << std::dec << "=" << count;
        }
        std::cout << std::endl;
    }
    for (const auto& entry : interpreter.interrupt_log()) {
        std::cout << entry << std::endl;
    }
    for (auto address : peeks) {
        std::cout << "peek 0x" << std::hex << address << " = 0x"
                  << interpreter.peek(address) << std::dec << std::endl;
    }
    std::cout << "gp=0x" << std::hex << interpreter.cpu().gpr[28] << std::dec
              << std::endl;
    if (!interpreter.irq_hot().empty()) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> hot;
        for (const auto& entry : interpreter.irq_hot()) {
            hot.emplace_back(entry.second, entry.first);
        }
        std::sort(hot.rbegin(), hot.rend());
        std::cout << "IRQ hot PCs:";
        for (std::size_t i = 0; i < hot.size() && i < 6; ++i) {
            std::cout << " 0x" << std::hex << hot[i].second
                      << std::dec << "=" << hot[i].first;
        }
        std::cout << std::endl;
    }
    for (const auto& entry : interpreter.watched()) {
        std::cout << "watch 0x" << std::hex << entry.first
                  << " = " << std::dec << entry.second << std::endl;
    }
    if (!interpreter.hot().empty()) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> hits;
        for (const auto& entry : interpreter.hot()) {
            hits.emplace_back(entry.second, entry.first);
        }
        std::sort(hits.rbegin(), hits.rend());
        std::cout << "hot PCs:";
        for (std::size_t i = 0; i < hits.size() && i < 8; ++i) {
            std::cout << " 0x" << std::hex << hits[i].second
                      << std::dec << "=" << hits[i].first;
        }
        std::cout << std::endl;
    }
    if (trace_cd) {
        for (const auto& entry : interpreter.dma3_log()) {
            std::cout << "dma3 " << entry << std::endl;
        }
    }
    for (const auto& entry : interpreter.trace_log()) {
        std::cout << entry << std::endl;
    }
    std::cout << "IRQ: SR=0x" << std::hex << interpreter.cop0_status()
              << " I_STAT=0x" << interpreter.irq_status() << " I_MASK=0x"
              << interpreter.irq_mask() << std::dec << "\n";
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
        case r6::StopReason::guest_break:
            std::cout << " (guest executed BREAK)\n";
            break;
        case r6::StopReason::interrupt_stalled:
            std::cout << " (interrupt routine did not return)\n";
            break;
    }


    {
        // A stack pointer outside main RAM silently loses every push and pop,
        // which shows up much later as a return to address zero.
        const auto sp = interpreter.cpu().gpr[29];
        const bool in_ram = sp >= 0x80000000u && sp < 0x80200000u;
        const bool in_scratchpad = sp >= 0x1F800000u && sp < 0x1F800400u;
        if (!in_ram && !in_scratchpad) {
            std::cout << "\nWarning: guest stack pointer 0x" << std::hex << sp
                      << " is outside main RAM and the scratchpad; pushes to it"
                      << " are discarded.\n"
                      << std::dec;
        }
    }

    {
        // Show the busiest hardware registers so unimplemented devices are
        // easy to spot during bring-up.
        std::vector<std::pair<std::uint32_t, std::uint64_t>> reads(
            interpreter.hardware_reads().begin(),
            interpreter.hardware_reads().end());
        std::sort(reads.begin(), reads.end(),
                  [](const auto& left, const auto& right) {
                      return left.second > right.second;
                  });
        std::cout << "\nHardware reads (top):";
        for (std::size_t index = 0; index < reads.size() && index < 8;
             ++index) {
            std::cout << " 0x" << std::hex << reads[index].first << std::dec
                      << " x" << reads[index].second;
        }
        std::cout << "\n";
    }

    const auto& gpu = services.gpu();
    std::cout << "GPU: " << gpu.rendered_primitives()
              << " primitives (" << gpu.fill_rectangles() << " fills), "
              << gpu.unsupported_commands() << " unsupported commands, "
              << gpu.filled_vram_pixels() << " filled VRAM pixels, display "
              << (gpu.display_enabled() ? "on" : "off") << " at "
              << gpu.display_area_x() << "," << gpu.display_area_y()
              << ", draw area " << gpu.draw_area_left() << ","
              << gpu.draw_area_top() << " to " << gpu.draw_area_right() << ","
              << gpu.draw_area_bottom() << "\n"
              << "GTE: " << interpreter.gte_commands() << " commands, "
              << interpreter.gte_unsupported_commands() << " unsupported"
              << interpreter.gte().unsupported_summary() << "\n"
              << "DMA: " << interpreter.dma_writes() << " register writes, "
              << interpreter.dma_lists() << " linked lists, "
              << interpreter.dma_words() << " words sent to the GPU\n"
              << "BIOS: " << services.bios().unhandled_call_count()
              << " unhandled vectors, " << interpreter.cd_sectors_read()
              << " disc sectors read\n"
              << "CD: status reads=" << interpreter.hardware().cdrom().status_reads()
              << " with response=" << interpreter.hardware().cdrom().response_seen()
              << " response reads=" << interpreter.hardware().cdrom().response_reads()
              << std::endl;
    for (const auto& entry : interpreter.watched_args()) {
        std::cout << entry << std::endl;
    }
    std::cout << "CD poll pcs:";
    for (auto poll_pc : interpreter.cdrom_polls()) {
        std::cout << " 0x" << std::hex << poll_pc << std::dec;
    }
    std::cout << std::endl;
    if (dump_stream) {
        const auto base = ram.read32(0x800C1510u & kRamMask);
        const auto capacity = ram.read32(0x800C1520u & kRamMask);
        const auto current = ram.read32(0x800C1514u & kRamMask);
        const auto ready = ram.read32(0x800C1518u & kRamMask);
        const auto consume = ram.read32(0x800C151Cu & kRamMask);
        const auto state = ram.read32(0x800C1508u & kRamMask);
        const auto used = ram.read32(0x800C1504u & kRamMask);
        std::cout << "STREAM: base=0x" << std::hex << base << std::dec
                  << " cap=" << capacity << " cur=" << current
                  << " ready=" << ready << " consume=" << consume
                  << " state=" << state << " used=" << used << "\n";
        std::cout << "STREAM ptrs: dest=0x" << std::hex
                  << ram.read32(0x800C14E0u & kRamMask) << " mode="
                  << ram.read32(0x800C14F4u & kRamMask) << " end=0x"
                  << ram.read32(0x800C14F8u & kRamMask) << " started="
                  << ram.read32(0x800C14FCu & kRamMask) << " cb=0x"
                  << ram.read32(0x800CF0D8u & kRamMask) << " dmaCb=0x"
                  << ram.read32(0x800B4394u & kRamMask) << " irq3=0x"
                  << ram.read32(0x800B2894u & kRamMask) << std::dec << "\n";
        for (std::uint32_t index = 0;
             index < capacity && index < 12; ++index) {
            const auto address = (base + index * 32u) & kRamMask;
            const auto word0 = ram.read32(address);
            const auto word1 = ram.read32((address + 4u) & kRamMask);
            std::cout << "  rec[" << index << "] state="
                      << (word0 & 0xFFFFu) << " span="
                      << ((word1 >> 16) & 0xFFFFu) << " w2="
                      << ((word0 >> 16) & 0xFFFFu) << " w8=0x"
                      << std::hex << ram.read32((address + 8u) & kRamMask)
                      << std::dec << " raw=";
            for (std::uint32_t word = 0; word < 8; ++word) {
                std::cout << std::hex
                          << ram.read32((address + word * 4u) & kRamMask)
                          << " ";
            }
            std::cout << std::dec << "\n";
        }
        for (std::uint32_t index = 0; index < 2; ++index) {
            const auto address = (base + capacity * 32u + index * 2016u) &
                                 kRamMask;
            std::cout << "  data[" << index << "]";
            for (std::uint32_t word = 0; word < 6; ++word) {
                std::cout << " 0x" << std::hex
                          << ram.read32((address + word * 4u) & kRamMask)
                          << std::dec;
            }
            std::cout << "\n";
        }
    }
    if (!interpreter.write_log().empty()) {
        for (const auto& entry : interpreter.write_log()) {
            std::cout << "ww " << entry << "\n";
        }
    }
    std::cout << "VBlank: " << interpreter.vblanks() << " ticks, "
              << interpreter.interrupts() << " interrupts delivered, "
              << interpreter.interrupt_overruns() << " handler overruns, "
              << interpreter.missing_vectors() << " pending vectors, "
              << interpreter.vblank_polls() << " polls, "
              << interpreter.hardware().cycles() << " cycles, next="
              << interpreter.vblank_next() << "\n";
    if (trace_cd) {
        std::cout << "CD events:" << std::endl;
        for (const auto& entry : interpreter.cdrom_log()) {
            std::cout << "  " << entry << std::endl;
        }
        std::cout << "IRQ events:" << std::endl;
        for (const auto& entry : interpreter.hardware().interrupt_log()) {
            std::cout << "  " << entry << std::endl;
        }
        std::cout << "CD registers:" << std::endl;
        for (const auto& entry : interpreter.cdrom_register_log()) {
            std::cout << "  " << entry << std::endl;
        }
    }
    if (!interpreter.vector_writes().empty()) {
        std::cout << "Vector writes:\n";
        for (const auto& write : interpreter.vector_writes()) {
            std::cout << "  0x" << std::hex << write.address << " <= 0x"
                      << write.value << " (" << std::dec << write.width
                      << "-bit) pc=0x" << std::hex << write.pc << std::dec
                      << "\n";
        }
    } else {
        std::cout << "Vector writes: none (the guest never wrote the vector)\n";
    }
    if (const auto entry_int = services.entry_int(); entry_int != 0) {
        std::cout << "EntryInt: 0x" << std::hex << entry_int << " pc=0x"
                  << ram.read32(entry_int & kRamMask)
                  << std::endl;
    }

    for (std::uint32_t opcode = 0; opcode < 0x100; ++opcode) {
        const auto count =
            gpu.unsupported_commands(static_cast<std::uint8_t>(opcode));
        if (count != 0) {
            std::cout << "  unsupported GP0 0x" << std::hex << opcode << std::dec
                      << " x" << count << "\n";
        }
    }

    for (std::uint32_t priority = 0; priority < 3; ++priority) {
        const auto entry = services.interrupt_entry(priority);
        if (entry == 0) continue;
        std::cout << "Interrupt entry " << priority << ": 0x" << std::hex
                  << entry << " next=0x" << ram.read32(entry & kRamMask)
                  << " routine=0x" << ram.read32((entry + 4) & kRamMask)
                  << std::dec << "\n";
    }

    if (!services.bios().unhandled_log().empty()) {
        std::cout << "\nUnhandled kernel calls (first "
                  << services.bios().unhandled_log().size() << "):\n";
        for (const auto& entry : services.bios().unhandled_log()) {
            std::cout << "  " << entry << "\n";
        }
    }

    if (!services.bios().tty_output().empty()) {
        std::cout << "\nGuest TTY output:\n" << services.bios().tty_output()
                  << "\n";
    }

    std::cout << "\nRecent call sites:";
    for (const auto site : interpreter.recent_calls()) {
        std::cout << " 0x" << std::hex << site << std::dec;
    }
    std::cout << "\n";

    std::cout << "\nLast PCs:";
    for (const auto pc : interpreter.recent_pcs()) {
        std::cout << " 0x" << std::hex << pc << std::dec;
    }
    std::cout << "\n";

    // Debug: dump allocator bins and suspect heap block
    {
        auto dump_words = [&](std::uint32_t base, std::uint32_t count, const char* label) {
            std::cout << "Dump " << label << " @0x" << std::hex << base << std::dec << "\n";
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t addr = base + i * 4u;
                std::uint32_t val = ram.read32(addr & kRamMask);
                std::cout << "  [0x" << std::hex << addr << "] = 0x" << val << std::dec << "\n";
            }
        };
        dump_words(0x800B55A0u, 16u, "bins");
        dump_words(0x80139AC0u, 16u, "heap@80139AC0");
        dump_words(0x80065340u, 32u, "code@65340");
        dump_words(0x80064F80u, 16u, "code@64f80");
        for (const auto& range : dump_ranges) {
            dump_words(range.first, range.second, "range");
        }
        for (const auto& entry : dump_pointers) {
            const auto target = ram.read32(entry.first & kRamMask);
            const auto label = "at*0x" + [&] {
                std::ostringstream text;
                text << std::hex << entry.first;
                return text.str();
            }();
            if (target == 0) {
                std::cout << "Dump " << label << " = 0x0 (no pointer)\n";
                continue;
            }
            std::cout << "Dump " << label << " -> 0x" << std::hex << target
                      << std::dec << "\n";
            dump_words(target, entry.second, label.c_str());
        }
    }

    if (!frame_path.empty()) {
        write_ppm(gpu.display_frame(), r6::SoftwareGpu::kDisplayWidth,
                  r6::SoftwareGpu::kDisplayHeight, frame_path);
        std::cout << "Wrote frame to " << frame_path.string() << "\n";
    }

    return result.reason == r6::StopReason::unsupported_instruction ? 1 : 0;
}
