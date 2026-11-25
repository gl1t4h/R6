// r6-native: execute the locally generated native translation.
//
// This front end boots the user's own executable, registers the generated
// basic blocks, and runs them through r6::FunctionDispatcher. With --compare it
// also runs the interpreter from the same checkpoint and checks the register
// file, HI/LO and all 2 MiB of RAM after every block. The interpreter is the
// reference implementation only; the generated code is the product target.
//
// The generated translation unit is a local, derived artifact. It is never
// committed: configure this target with -DR6_GENERATED_TRANSLATION=<path>.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "r6/bios_services.h"
#include "r6/boot_image.h"
#include "r6/function_dispatch.h"
#include "r6/memory.h"
#include "r6/mips_interpreter.h"
#include "r6/native_runtime.h"
#include "r6/platform_services.h"
#include "r6/ram_snapshot.h"
#include "r6/windows_platform_services.h"

#ifdef R6_HAS_GENERATED_TRANSLATION
namespace r6_generated {
void register_functions(r6::FunctionDispatcher& dispatcher);
std::size_t block_count();
}  // namespace r6_generated
#endif

namespace {

// The differential harness gives each block a fresh platform instance so the
// native bus and the interpreter start from the same hardware and kernel
// state. A real run can keep one alive across blocks later.
class NativePlatformServices final : public r6::PlatformServices {
public:
    std::uint16_t read_controller(std::size_t) override { return 0; }
    void submit_audio_samples(const std::int16_t*, std::size_t) override {}
    void present_frame(const std::uint32_t*, std::size_t) override {}
    std::uint32_t read_timer(std::size_t) override { return 0; }
    void write_gpu_register(std::uint32_t, std::uint32_t) override {}
    std::uint32_t read_gpu_status() override { return 0; }
    std::uint32_t read_gpu_data() override { return 0; }
    bool read_disc_sector(std::uint32_t, std::uint8_t*,
                          std::size_t) override {
        return false;
    }
    bool bios_call(std::uint32_t pc, r6::CpuState& cpu,
                   r6::Memory& memory) override {
        r6::BiosTable table{};
        if (!r6::BiosServices::trap_table(pc, table)) return false;
        bios_.dispatch(table, cpu, memory);
        return true;
    }

private:
    r6::BiosServices bios_;
};

constexpr std::uint32_t kRamSize = 2u * 1024u * 1024u;
constexpr std::uint32_t kRamMask = kRamSize - 1u;
constexpr std::uint32_t kDefaultStack = 0x801FFFF0u;
constexpr std::uint32_t kCrStackGlobal = 0x800B3E70u;

struct Boot {
    std::filesystem::path bin;
    r6::boot::ExeImage image;
    std::unique_ptr<r6::WindowsPlatformServices> services;
    r6::Memory ram{kRamSize, true};
    r6::CpuState initial{};
    std::string exe_name;
};

std::string hex32(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

bool load_boot(const std::filesystem::path& input, Boot& boot, std::string& error) {
    boot.bin = r6::boot::resolve_bin(input);
    std::error_code filesystem_error;
    if (!std::filesystem::exists(boot.bin, filesystem_error)) {
        error = "disc image not found: " + boot.bin.string();
        return false;
    }

    boot.services = std::make_unique<r6::WindowsPlatformServices>(boot.bin);
    if (!boot.services->disc_open()) {
        error = input.string() + ": " + boot.services->disc_error();
        return false;
    }

    const auto files = r6::boot::list_root(*boot.services);
    if (files.empty()) {
        error = "no ISO9660 volume found in " + boot.bin.string();
        return false;
    }

    std::string exe_name;
    for (const auto& file : files) {
        if (r6::boot::upper(file.name) == "SYSTEM.CNF") {
            exe_name =
                r6::boot::boot_name(r6::boot::read_file(*boot.services, file));
        }
    }
    if (exe_name.empty()) {
        for (const auto& file : files) {
            if (r6::boot::upper(file.name).find("SLUS") != std::string::npos) {
                exe_name = file.name;
                break;
            }
        }
    }
    if (exe_name.empty()) {
        error = "could not determine the boot executable";
        return false;
    }

    const r6::boot::DiscFile* executable = nullptr;
    for (const auto& file : files) {
        if (r6::boot::upper(file.name) == r6::boot::upper(exe_name)) {
            executable = &file;
            break;
        }
    }
    if (executable == nullptr) {
        error = exe_name + " is not present in the image";
        return false;
    }

    if (!r6::boot::parse_exe(
            r6::boot::read_file(*boot.services, *executable), boot.image)) {
        error = exe_name + " has no valid PS-X EXE header";
        return false;
    }
    boot.exe_name = executable->name;

    for (std::size_t index = 0; index < boot.image.payload.size(); ++index) {
        boot.ram.write8(
            (boot.image.load_address + static_cast<std::uint32_t>(index)) &
                kRamMask,
            boot.image.payload[index]);
    }

    // The C runtime reads its stack limit from .data and computes a stack
    // outside RAM. The retail loader supplies a usable one, so correct the
    // global before translated code reads it, exactly as the oracle does.
    const auto cr_stack = boot.ram.read32(kCrStackGlobal & kRamMask);
    const bool cr_stack_in_ram =
        cr_stack >= 0x80000000u && cr_stack < 0x80200000u;
    if (!cr_stack_in_ram) {
        boot.ram.write32(kCrStackGlobal & kRamMask, kDefaultStack);
    }

    boot.initial.pc = boot.image.entry;
    boot.initial.gpr[28] = boot.image.gp;
    boot.initial.gpr[29] =
        boot.image.stack_base != 0
            ? boot.image.stack_base
            : (cr_stack_in_ram ? cr_stack : kDefaultStack);
    boot.initial.gpr[31] = 0;
    return true;
}

bool same_cpu(const r6::CpuState& left, const r6::CpuState& right) {
    if (left.hi != right.hi || left.lo != right.lo || left.pc != right.pc) {
        return false;
    }
    return std::memcmp(left.gpr, right.gpr, sizeof(left.gpr)) == 0;
}

void print_cpu_difference(
    std::uint32_t block,
    const r6::CpuState& native,
    const r6::CpuState& oracle) {
    std::cout << "Mismatch after block " << block << " (native PC "
              << hex32(native.pc) << ", oracle PC " << hex32(oracle.pc)
              << ")\n";
    for (std::size_t index = 0; index < 32; ++index) {
        if (native.gpr[index] != oracle.gpr[index]) {
            std::cout << "  $r" << index << ": native " << hex32(native.gpr[index])
                      << ", oracle " << hex32(oracle.gpr[index]) << "\n";
        }
    }
    if (native.hi != oracle.hi) {
        std::cout << "  HI: native " << hex32(native.hi) << ", oracle "
                  << hex32(oracle.hi) << "\n";
    }
    if (native.lo != oracle.lo) {
        std::cout << "  LO: native " << hex32(native.lo) << ", oracle "
                  << hex32(oracle.lo) << "\n";
    }
}

void print_ram_difference(
    const std::vector<std::uint8_t>& native,
    const std::vector<std::uint8_t>& oracle) {
    for (std::size_t index = 0; index < native.size(); ++index) {
        if (native[index] == oracle[index]) {
            continue;
        }
        const auto address = static_cast<std::uint32_t>(index);
        std::cout << "  RAM " << hex32(0x80000000u | address) << ": native 0x"
                  << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(native[index]) << ", oracle 0x"
                  << std::setw(2) << static_cast<unsigned>(oracle[index])
                  << std::dec << std::setfill(' ') << "\n";
        break;
    }
}

bool is_bios_entry(std::uint32_t pc) {
    const auto physical = pc & 0x1FFFFFFFu;
    return physical == 0xA0u || physical == 0xB0u || physical == 0xC0u;
}

void report_stop(const r6::CpuState& cpu) {
    if (is_bios_entry(cpu.pc)) {
        std::cout << "Native stop: BIOS entry " << hex32(cpu.pc & 0x1FFFFFFFu)
                  << " (t1=" << hex32(cpu.gpr[9])
                  << ") reached; this is the platform-service seam\n";
        return;
    }
    std::cout << "Native stop: no translated block at " << hex32(cpu.pc) << "\n";
}

int run_native(
    const std::filesystem::path& input,
    const std::filesystem::path& snapshot_path,
    std::uint32_t entry_override,
    std::uint64_t max_blocks,
    std::uint64_t oracle_steps,
    bool compare,
    bool verbose) {
#ifdef R6_HAS_GENERATED_TRANSLATION
    Boot boot;
    const bool snapshot_mode = !snapshot_path.empty();
    std::string error;
    if (snapshot_mode) {
        if (!r6::read_snapshot(snapshot_path, boot.initial, boot.ram)) {
            std::cerr << "r6-native: could not read snapshot " << snapshot_path
                      << "\n";
            return 1;
        }
        boot.exe_name = snapshot_path.filename().string();
        boot.image.entry = boot.initial.pc;
        boot.image.gp = boot.initial.gpr[28];
    } else if (!load_boot(input, boot, error)) {
        std::cerr << "r6-native: " << error << "\n";
        return 1;
    }

    // The interpreter resets COP0 with SR=0x401 at construction. Snapshots
    // do not carry COP0 yet, so the native checkpoint starts from that same
    // reset value and the oracle is given it before every block.
    boot.initial.cop0[12] = 0x00000401u;

    r6::FunctionDispatcher dispatcher;
    r6_generated::register_functions(dispatcher);

    const auto start_pc =
        entry_override != 0 ? entry_override : boot.initial.pc;
    std::cout << "R6 native translation runner\n";
    if (snapshot_mode) {
        std::cout << "Snapshot: " << snapshot_path.string() << "\n";
    } else {
        std::cout << "Disc: " << boot.bin.filename().string() << "\n"
                  << "EXE:  " << boot.exe_name << " (" << boot.image.size
                  << " bytes)\n";
    }
    std::cout << "Entry " << hex32(start_pc) << ", GP "
              << hex32(boot.initial.gpr[28]) << ", SP "
              << hex32(boot.initial.gpr[29]) << "\n"
              << "Translated blocks: " << r6_generated::block_count() << "\n";
    if (!dispatcher.contains(start_pc)) {
        std::cout << "Native stop: no translated block at entry "
                  << hex32(start_pc) << "\n";
        return 1;
    }

    r6::Memory native_ram = boot.ram;
    r6::CpuState native = boot.initial;
    native.pc = start_pc;

    r6::Memory oracle_ram = boot.ram;
    std::uint64_t matched = 0;
    bool failed = false;
    bool reached_stop = false;

    for (std::uint64_t block = 0; block < max_blocks; ++block) {
        const bool bios = is_bios_entry(native.pc);
        if (!bios && !dispatcher.contains(native.pc)) {
            report_stop(native);
            reached_stop = true;
            break;
        }

        const auto before_cpu = native;
        const auto before_ram = native_ram.snapshot();
        NativePlatformServices native_services;
        try {
            if (bios) {
                if (!native_services.bios_call(native.pc, native, native_ram)) {
                    report_stop(native);
                    reached_stop = true;
                    break;
                }
                native.pc = native.gpr[31];
            } else {
                native.pc =
                    dispatcher.step(native, native_ram, native_services);
            }
        } catch (const std::exception& exception) {
            std::cout << "Native fault at " << hex32(native.pc) << ": "
                      << exception.what() << "\n";
            return 1;
        }
        if (verbose) {
            std::cout << "  block " << block << ": " << hex32(before_cpu.pc)
                      << " -> " << hex32(native.pc) << "\n";
        }

        if (compare) {
            oracle_ram.restore(before_ram);
            NativePlatformServices oracle_services;
            r6::MipsInterpreter reference(oracle_ram, oracle_services);
            auto& oracle = reference.cpu();
            oracle = before_cpu;
            reference.set_cop0(before_cpu.cop0);

            bool reached = false;
            for (std::uint64_t step = 0; step < oracle_steps; ++step) {
                const auto result = reference.run(1);
                if (result.reason != r6::StopReason::step_budget) {
                    std::cout << "Oracle stopped at " << hex32(result.pc)
                              << " with reason "
                              << static_cast<int>(result.reason)
                              << " on block " << block << " starting at "
                              << hex32(before_cpu.pc) << " before reaching "
                              << hex32(native.pc) << "\n";
                    return 1;
                }
                if (oracle.pc == native.pc) {
                    reached = true;
                    break;
                }
            }
            if (!reached) {
                std::cout << "Oracle did not reach " << hex32(native.pc)
                          << " from " << hex32(before_cpu.pc) << " on block "
                          << block << " within " << oracle_steps
                          << " instructions\n";
                return 1;
            }
            if (!same_cpu(native, oracle)) {
                print_cpu_difference(static_cast<std::uint32_t>(block), native,
                                     oracle);
                failed = true;
                break;
            }
            bool cop0_mismatch = false;
            for (std::size_t index = 0; index < 32; ++index) {
                if (native.cop0[index] == reference.cop0(index)) continue;
                std::cout << "  COP0 r" << index << ": native "
                          << hex32(native.cop0[index]) << ", oracle "
                          << hex32(reference.cop0(index)) << "\n";
                cop0_mismatch = true;
                break;
            }
            if (cop0_mismatch) {
                std::cout << "COP0 mismatch after block " << block
                          << " (native PC " << hex32(native.pc) << ")\n";
                failed = true;
                break;
            }
            if (native_ram.snapshot() != oracle_ram.snapshot()) {
                print_ram_difference(native_ram.snapshot(), oracle_ram.snapshot());
                std::cout << "RAM mismatch after block " << block << " (native PC "
                          << hex32(native.pc) << ")\n";
                failed = true;
                break;
            }
            ++matched;
        }
    }

    if (!reached_stop) {
        if (native.pc == start_pc && matched == 0) {
            std::cout << "Native stop: block budget reached at start\n";
        } else if (dispatcher.contains(native.pc)) {
            std::cout << "Native stop: block budget reached at "
                      << hex32(native.pc) << "\n";
        }
    }

    if (compare) {
        std::cout << "Differential check: " << matched << " block(s) matched"
                  << (failed ? " before the first mismatch" : "")
                  << " (registers, HI/LO, COP0, 2 MiB RAM)\n";
    }
    return failed ? 1 : 0;
#else
    (void)input;
    (void)snapshot_path;
    (void)entry_override;
    (void)max_blocks;
    (void)oracle_steps;
    (void)compare;
    (void)verbose;
    std::cerr << "r6-native: no generated translation unit was configured.\n"
              << "Generate one outside the repository, then configure with:\n"
              << "  -DR6_GENERATED_TRANSLATION=<path to generated .cpp>\n";
    return 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path input;
    std::filesystem::path snapshot_path;
    std::uint32_t entry_override = 0;
    std::uint64_t blocks = 2048;
    std::uint64_t oracle_steps = 100000;
    bool compare = true;
    bool verbose = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--entry" && index + 1 < argc) {
            entry_override = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 0));
        } else if (argument == "--snapshot" && index + 1 < argc) {
            snapshot_path = argv[++index];
        } else if (argument == "--blocks" && index + 1 < argc) {
            blocks = std::stoull(argv[++index]);
        } else if (argument == "--oracle-steps" && index + 1 < argc) {
            oracle_steps = std::stoull(argv[++index]);
        } else if (argument == "--no-compare") {
            compare = false;
        } else if (argument == "--verbose") {
            verbose = true;
        } else if (input.empty()) {
            input = argument;
        }
    }

    if (input.empty() && snapshot_path.empty()) {
        std::cerr
            << "usage: r6-native <game.cue|game.bin|game.chd> [--entry 0xADDR]"
               " [--blocks N] [--oracle-steps N] [--no-compare] [--verbose]\n"
               "       r6-native --snapshot RAM.snap [--entry 0xADDR]"
               " [--blocks N]\n"
            << "Runs the locally generated native translation and compares it"
               " against the interpreter after every block.\n";
        return 2;
    }
    return run_native(input, snapshot_path, entry_override, blocks,
                      oracle_steps, compare, verbose);
}
