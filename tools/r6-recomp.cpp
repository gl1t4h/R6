// r6-recomp: ahead-of-time translator from PlayStation 1 MIPS to native C++.
//
// This is the full-image companion to tools/generate-native-stub.py. It emits
// the same block-threaded ABI that r6-native already consumes:
//
//   std::uint32_t block_XXXXXXXX(r6::CpuState& cpu, r6::NativeBus& bus);
//   namespace r6_generated { void register_functions(...); }
//
// so the generated file can be used with -DR6_GENERATED_TRANSLATION exactly
// like a stub translation. The difference is the input and the coverage: it
// reads the user's own .chd/.cue/.bin directly, extracts the boot executable,
// discovers reachable code, and translates every reachable basic block instead
// of a hand-sized slice.
//
// Nothing is executed by an interpreter at runtime. An address with no
// translated block is reported by the runner as a recompilation miss.
//
// Usage:
//   r6-recomp <image.chd|image.cue|image.bin|SLUS_xxx> <output.cpp>
//             [--name SpiderMan] [--extra-leaders FILE]
//
// The output is derived from game code. Write it outside the repository.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "r6/chd_image.h"

namespace {

constexpr std::uint32_t kSectorBytes = 2048;
constexpr std::uint32_t kRawSectorBytes = 2352;

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

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    for (auto& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension;
}

// ---------------------------------------------------------------- disc input

class Disc {
public:
    bool open(const std::filesystem::path& path) {
        const auto extension = lower_extension(path);
        auto image = path;
        if (extension == ".cue") {
            image = resolve_cue(path);
            if (image.empty()) return false;
        }
        if (lower_extension(image) == ".chd") {
            std::string error;
            if (!chd_.open(image, error)) {
                std::cerr << error << "\n";
                return false;
            }
            return true;
        }
        bin_ = image;
        return static_cast<bool>(std::ifstream(bin_, std::ios::binary));
    }

    bool read(std::uint32_t lba, std::uint8_t* destination) {
        if (chd_.is_open()) {
            return chd_.read_user_sector(lba, destination, kSectorBytes);
        }
        std::ifstream in(bin_, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(lba) * kRawSectorBytes);
        std::vector<std::uint8_t> raw(kRawSectorBytes, 0);
        in.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(raw.size()));
        if (in.gcount() < static_cast<std::streamsize>(raw.size())) return false;
        std::memcpy(destination, raw.data() + 24, kSectorBytes);
        return true;
    }

private:
    static std::filesystem::path resolve_cue(const std::filesystem::path& cue) {
        std::ifstream in(cue);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("FILE", 0) == std::string::npos) continue;
            const auto first = line.find('"');
            if (first == std::string::npos) continue;
            const auto second = line.find('"', first + 1);
            if (second == std::string::npos) continue;
            return cue.parent_path() /
                   line.substr(first + 1, second - first - 1);
        }
        return {};
    }

    r6::ChdImage chd_;
    std::filesystem::path bin_;
};

struct DiscFile {
    std::uint32_t lba{0};
    std::uint32_t size{0};
    std::string name;
};

std::vector<DiscFile> list_root(Disc& disc) {
    std::vector<DiscFile> files;
    std::uint8_t sector[kSectorBytes];
    if (!disc.read(16, sector)) return files;
    if (sector[0] != 1 || std::memcmp(sector + 1, "CD001", 5) != 0) {
        return files;
    }
    const auto root_lba = le32(sector + 156 + 2);
    const auto root_size = le32(sector + 156 + 10);
    for (std::uint32_t index = 0; index * kSectorBytes < root_size; ++index) {
        if (!disc.read(root_lba + index, sector)) break;
        std::size_t offset = 0;
        while (offset + 33 <= kSectorBytes) {
            const auto length = sector[offset];
            if (length == 0) break;
            if (offset + length > kSectorBytes) break;
            const auto* record = sector + offset;
            const auto name_length = record[32];
            if (name_length == 0 || offset + 33 + name_length > kSectorBytes) {
                break;
            }
            std::string name(reinterpret_cast<const char*>(record + 33),
                             name_length);
            const auto semicolon = name.find(';');
            if (semicolon != std::string::npos) name.resize(semicolon);
            if ((record[25] & 0x02u) == 0) {
                files.push_back({le32(record + 2), le32(record + 10), name});
            }
            offset += length;
        }
    }
    return files;
}

std::vector<std::uint8_t> read_disc_file(Disc& disc, const DiscFile& file) {
    std::vector<std::uint8_t> data(file.size, 0);
    std::uint8_t sector[kSectorBytes];
    std::size_t written = 0;
    for (std::uint32_t index = 0; written < data.size(); ++index) {
        if (!disc.read(file.lba + index, sector)) break;
        const auto chunk =
            std::min<std::size_t>(kSectorBytes, data.size() - written);
        std::memcpy(data.data() + written, sector, chunk);
        written += chunk;
    }
    return data;
}

std::string boot_name(const std::vector<std::uint8_t>& cnf) {
    const std::string text(cnf.begin(), cnf.end());
    const auto boot = text.find("BOOT");
    if (boot == std::string::npos) return {};
    const auto equals = text.find('=', boot);
    if (equals == std::string::npos) return {};
    auto begin = equals + 1;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    auto end = text.find_first_of("\r\n", begin);
    if (end == std::string::npos) end = text.size();
    auto name = text.substr(begin, end - begin);
    const auto colon = name.rfind(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    while (!name.empty() && (name.front() == '\\' || name.front() == '/')) {
        name.erase(name.begin());
    }
    const auto semicolon = name.find(';');
    if (semicolon != std::string::npos) name.resize(semicolon);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) {
        name.pop_back();
    }
    return name;
}

// ------------------------------------------------------------------- image

struct Image {
    std::uint32_t entry{0};
    std::uint32_t load{0};
    std::uint32_t size{0};
    std::vector<std::uint32_t> words;
    std::string name;
};

bool parse_exe(const std::vector<std::uint8_t>& data, Image& image) {
    if (data.size() < 0x800 || std::memcmp(data.data(), "PS-X EXE", 8) != 0) {
        return false;
    }
    image.entry = le32(data.data() + 0x10);
    image.load = le32(data.data() + 0x18);
    image.size = le32(data.data() + 0x1C);
    const auto available = data.size() - 0x800;
    const auto bytes = std::min<std::size_t>(image.size, available);
    image.words.assign((bytes + 3) / 4, 0);
    std::memcpy(image.words.data(), data.data() + 0x800, bytes);
    return image.entry != 0 && image.size != 0;
}

bool load_image(const std::filesystem::path& path, Image& image,
                std::string& error) {
    if (lower_extension(path) == ".exe") {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + path.string();
            return false;
        }
        std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
        image.name = path.stem().string();
        if (!parse_exe(data, image)) {
            error = "not a PS-X EXE";
            return false;
        }
        return true;
    }

    Disc disc;
    if (!disc.open(path)) {
        error = "cannot open " + path.string();
        return false;
    }
    const auto files = list_root(disc);
    const DiscFile* cnf = nullptr;
    for (const auto& file : files) {
        if (upper(file.name) == "SYSTEM.CNF") cnf = &file;
    }
    if (cnf == nullptr) {
        error = "no SYSTEM.CNF in the ISO9660 root";
        return false;
    }
    const auto exe_name = boot_name(read_disc_file(disc, *cnf));
    const DiscFile* exe = nullptr;
    for (const auto& file : files) {
        if (upper(file.name) == upper(exe_name)) exe = &file;
    }
    if (exe == nullptr) {
        error = "SYSTEM.CNF names " + exe_name + ", which is not on the disc";
        return false;
    }
    image.name = exe_name;
    if (!parse_exe(read_disc_file(disc, *exe), image)) {
        error = exe_name + " has no PS-X EXE header";
        return false;
    }
    return true;
}

// --------------------------------------------------------------- decoding

constexpr std::uint32_t reg(std::uint32_t instruction, std::uint32_t shift) {
    return (instruction >> shift) & 0x1Fu;
}

constexpr std::uint32_t immediate(std::uint32_t instruction) {
    return instruction & 0xFFFFu;
}

std::uint32_t sign_extend16(std::uint32_t value) {
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFFu)));
}

std::string hex32(std::uint32_t value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08Xu", value);
    return buffer;
}

std::string hex16(std::uint32_t value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%04Xu", value);
    return buffer;
}

enum class Flow {
    none,
    branch,
    call,      // jal
    jump,      // j
    indirect,  // jr/jalr
    terminal,  // break, syscall, illegal
};

struct Decoded {
    Flow flow{Flow::none};
    std::uint32_t target{0};
    bool link{false};
    bool bc2{false};
    bool bc2_taken{false};
};

Decoded decode_flow(std::uint32_t instruction, std::uint32_t pc) {
    Decoded decoded;
    const auto opcode = instruction >> 26;
    const auto rt = reg(instruction, 16);
    switch (opcode) {
        case 0x00: {
            const auto function = instruction & 0x3Fu;
            if (function == 0x08 || function == 0x09) {
                decoded.flow = Flow::indirect;
                decoded.link = function == 0x09;
            }
            // syscall is libetc's critical-section entry/exit and is emitted
            // as a bus call; break ends the program and is not translatable.
            if (function == 0x0D) decoded.flow = Flow::terminal;
            break;
        }
        case 0x01:
            if (rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11) {
                decoded.flow = Flow::branch;
                decoded.link = rt >= 0x10;
            }
            break;
        case 0x02:
            decoded.flow = Flow::jump;
            decoded.target =
                (pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
            break;
        case 0x03:
            decoded.flow = Flow::call;
            decoded.target =
                (pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
            break;
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
            decoded.flow = Flow::branch;
            break;
        case 0x12:
            // BC2F/BC2T test the GTE condition flag, which reads zero on
            // hardware: BC2F falls through and BC2T always branches.
            if (reg(instruction, 21) == 0x08) {
                decoded.flow = Flow::branch;
                decoded.bc2 = true;
                decoded.bc2_taken = rt != 0;
                decoded.target =
                    rt == 0 ? pc + 8
                            : pc + 4 +
                                  (sign_extend16(immediate(instruction)) << 2);
            }
            break;
        default:
            break;
    }
    if (decoded.flow == Flow::branch && !decoded.bc2) {
        decoded.target = pc + 4 + (sign_extend16(immediate(instruction)) << 2);
    }
    if (decoded.flow == Flow::terminal) decoded.target = 0;
    return decoded;
}

// ------------------------------------------------------------- translation

class Recompiler {
public:
    Recompiler(Image image, std::string name)
        : image_(std::move(image)), name_(std::move(name)) {}

    bool in_image(std::uint32_t pc) const {
        if (pc < image_.load) return false;
        const auto offset = pc - image_.load;
        return offset < image_.size;
    }

    std::uint32_t word(std::uint32_t pc) const {
        const auto index = (pc - image_.load) / 4;
        return index < image_.words.size() ? image_.words[index] : 0;
    }

    void discover(const std::vector<std::uint32_t>& extra, bool seed_pointers) {
        for (auto pc : extra) seed(pc);
        seed(image_.entry);

        // Code addresses stored in tables are how the title reaches its mode
        // entries and callbacks: those functions have no direct call site.
        // Seeding the table words costs a few thousand blocks and turns a
        // runtime recompilation miss into translated code.
        if (seed_pointers) {
            for (std::uint32_t pc = image_.load; in_image(pc); pc += 4) {
                const auto value = word(pc);
                if (value >= image_.load && value < image_.load + image_.size &&
                    (value & 3u) == 0 && value != 0) {
                    seed(value);
                }
                // Runtime-installed callbacks are written into structures the
                // translator cannot see. A function prologue is still
                // recognisable (`addiu sp,sp,-N`), and the CD/level callbacks
                // are ordinary functions like any other.
                const auto opcode = value >> 26;
                const auto frame = sign_extend16(value & 0xFFFFu);
                if (opcode == 0x09 && reg(value, 21) == 29 &&
                    reg(value, 16) == 29 &&
                    static_cast<std::int32_t>(frame) < 0) {
                    seed(pc);
                }
            }
        }

        // Every direct call in the image is a function entry point.
        for (std::uint32_t pc = image_.load; in_image(pc); pc += 4) {
            const auto instruction = word(pc);
            if ((instruction >> 26) == 0x03) {
                const auto target = (pc & 0xF0000000u) |
                                    ((instruction & 0x03FFFFFFu) << 2);
                seed(target);
            }
        }

        // Resolve jr targets built from lui/addiu/ori constants: dispatch
        // trampolines and the libc startup hand-offs use that pattern.
        std::uint32_t constants[32]{};
        bool known[32]{};
        for (std::uint32_t pc = image_.load; in_image(pc); pc += 4) {
            const auto instruction = word(pc);
            const auto opcode = instruction >> 26;
            const auto rs = reg(instruction, 21);
            const auto rt = reg(instruction, 16);
            if (opcode == 0x0F) {
                constants[rt] = immediate(instruction) << 16;
                known[rt] = rt != 0;
            } else if (opcode == 0x09) {
                if (known[rs] && rt != 0) {
                    constants[rt] = constants[rs] +
                                    sign_extend16(immediate(instruction));
                    known[rt] = true;
                } else if (rt != 0) {
                    known[rt] = false;
                }
            } else if (opcode == 0x0D) {
                if (known[rs] && rt != 0) {
                    constants[rt] = constants[rs] | immediate(instruction);
                    known[rt] = true;
                } else if (rt != 0) {
                    known[rt] = false;
                }
            } else if (opcode == 0x00 && (instruction & 0x3Fu) == 0x08) {
                if (known[rs]) seed(constants[rs]);
                std::fill(std::begin(known), std::end(known), false);
            } else if (opcode >= 0x04 && opcode <= 0x07) {
                std::fill(std::begin(known), std::end(known), false);
            } else if (opcode != 0x00 && rt != 0) {
                known[rt] = false;
            }
        }

        while (!queue_.empty()) {
            const auto pc = queue_.back();
            queue_.pop_back();
            walk(pc);
        }
        // A delay slot is never a block entry: it has to execute with its
        // branch. This matches generate-native-stub.py's leader filter.
        for (std::uint32_t pc = image_.load; in_image(pc); pc += 4) {
            const auto decoded = decode_flow(word(pc), pc);
            if (decoded.flow == Flow::branch || decoded.flow == Flow::call ||
                decoded.flow == Flow::jump || decoded.flow == Flow::indirect) {
                delay_slots_.insert(pc + 4);
            }
        }
        for (auto pc : delay_slots_) leaders_.erase(pc);
    }

    void emit(std::ostream& out) {
        std::uint64_t instructions = 0;
        std::uint64_t unresolved = 0;
        for (std::uint32_t pc = image_.load; in_image(pc); pc += 4) {
            const auto decoded = decode_flow(word(pc), pc);
            if (decoded.flow == Flow::indirect) ++unresolved;
        }

        out << "// GENERATED LOCALLY FROM A USER-SUPPLIED PS-X EXE.\n"
            << "// Do not commit or distribute this file.\n"
            << "#include \"r6/native_runtime.h\"\n\n"
            << "#include <cstddef>\n"
            << "#include <cstdint>\n\n"
            << "namespace r6_generated {\n\n";

        for (auto pc : leaders_) {
            const auto addresses = block_addresses(pc);
            out << "static std::uint32_t body_" << std::setw(8) << std::setfill('0')
                << std::hex << pc << std::dec
                << "(r6::CpuState& cpu, r6::NativeBus& bus, int entry) {\n"
                << "    (void)bus;\n"
                << "    // Block at 0x" << std::setw(8) << std::setfill('0')
                << std::hex << pc << std::dec << "\n";
            if (addresses.size() > 1) {
                out << "    switch (entry) {\n";
                for (std::size_t index = 1; index < addresses.size(); ++index) {
                    out << "        case " << index << ": goto resume_"
                        << std::setw(8) << std::setfill('0') << std::hex
                        << addresses[index] << std::dec << ";\n";
                }
                out << "        default: break;\n    }\n";
            }
            instructions += emit_block(out, pc);
            out << "}\n\n";
            out << "std::uint32_t block_" << std::setw(8) << std::setfill('0')
                << std::hex << pc << std::dec
                << "(r6::CpuState& cpu, r6::NativeBus& bus) {\n"
                << "    return body_" << std::setw(8) << std::setfill('0')
                << std::hex << pc << std::dec << "(cpu, bus, 0);\n}\n\n";
            for (std::size_t index = 1; index < addresses.size(); ++index) {
                out << "std::uint32_t enter_" << std::setw(8) << std::setfill('0')
                    << std::hex << addresses[index] << std::dec
                    << "(r6::CpuState& cpu, r6::NativeBus& bus) {\n"
                    << "    return body_" << std::setw(8) << std::setfill('0')
                    << std::hex << pc << std::dec << "(cpu, bus, " << index
                    << ");\n}\n\n";
            }
        }

        out << "std::size_t block_count() {\n"
            << "    return " << leaders_.size() << "u;\n"
            << "}\n\n"
            << "void register_functions(r6::NativeBlockDispatcher& dispatcher) {\n";
        for (auto pc : leaders_) {
            out << "    dispatcher.register_function(" << hex32(pc)
                << ", &block_" << std::setw(8) << std::setfill('0') << std::hex
                << pc << std::dec << ");\n";
            for (const auto address : block_addresses(pc)) {
                if (address == pc) continue;
                out << "    dispatcher.register_function(" << hex32(address)
                    << ", &enter_" << std::setw(8) << std::setfill('0')
                    << std::hex << address << std::dec << ");\n";
            }
        }
        out << "}\n\n}  // namespace r6_generated\n";

        std::cerr << "r6-recomp: " << image_.name << "\n"
                  << "  entry        0x" << std::hex << image_.entry << std::dec
                  << "\n  load         0x" << std::hex << image_.load << std::dec
                  << "\n  size         " << image_.size << " bytes ("
                  << image_.size / 4 << " words)\n"
                  << "  blocks       " << leaders_.size() << "\n"
                  << "  instructions " << instructions << "\n"
                  << "  jr/jalr sites " << unresolved
                  << " (resolved at runtime)\n";
    }

private:
    void seed(std::uint32_t pc) {
        if (!in_image(pc)) return;
        if (!leaders_.insert(pc).second) return;
        queue_.push_back(pc);
    }

    void walk(std::uint32_t start) {
        std::uint32_t pc = start;
        for (;;) {
            if (!in_image(pc)) return;
            const auto decoded = decode_flow(word(pc), pc);
            if (decoded.flow == Flow::branch || decoded.flow == Flow::call) {
                seed(decoded.target);
                seed(pc + 8);
                return;
            }
            if (decoded.flow == Flow::jump) {
                seed(decoded.target);
                return;
            }
            if (decoded.flow == Flow::indirect) {
                // A jalr returns to the instruction after its delay slot; that
                // continuation is real code even though no branch targets it.
                // This is how the title's 13-entry mode dispatcher loops.
                seed(pc + 8);
                return;
            }
            if (decoded.flow == Flow::terminal) return;
            pc += 4;
            if (leaders_.count(pc) != 0) return;
        }
    }

    // Instruction addresses a block covers, in order. Every interior
    // instruction gets its own dispatch entry so an interrupt taken mid-block
    // can resume exactly where it left off.
    std::vector<std::uint32_t> block_addresses(std::uint32_t start) const {
        std::vector<std::uint32_t> addresses;
        std::uint32_t pc = start;
        for (;;) {
            if (!in_image(pc)) break;
            if (pc != start && leaders_.count(pc) != 0) break;
            addresses.push_back(pc);
            const auto decoded = decode_flow(word(pc), pc);
            if (decoded.flow != Flow::none) break;
            pc += 4;
        }
        return addresses;
    }

    std::uint64_t emit_block(std::ostream& out, std::uint32_t start) {
        std::uint64_t count = 0;
        std::uint32_t pc = start;
        for (;;) {
            if (!in_image(pc)) {
                out << "    cpu.pc = " << hex32(pc) << ";\n"
                    << "    return cpu.pc;\n";
                return count;
            }
            if (pc != start && leaders_.count(pc) != 0) {
                out << "    cpu.pc = " << hex32(pc) << ";\n"
                    << "    return cpu.pc;\n";
                return count;
            }

            const auto instruction = word(pc);
            const auto decoded = decode_flow(instruction, pc);
            if (pc != start) {
                out << "resume_" << std::setw(8) << std::setfill('0') << std::hex
                    << pc << std::dec << ":\n";
            }
            out << "    {\n";
            out << "    if (bus.event_tick(cpu)) return cpu.pc;\n";
            out << "    // 0x" << std::setw(8) << std::setfill('0') << std::hex
                << pc << std::dec << ": " << std::hex << std::setw(8)
                << std::setfill('0') << instruction << std::dec << "\n";

            if (decoded.flow != Flow::none) {
                emit_control(out, instruction, pc, decoded);
                out << "    }\n";
                return count + 1;
            }

            emit_body(out, instruction, pc);
            out << "    cpu.pc += 4;\n"
                << "    bus.advance_instruction();\n"
                << "    }\n";
            ++count;
            pc += 4;
        }
    }

    void emit_delay_slot(std::ostream& out, std::uint32_t address) {
        if (!in_image(address)) {
            out << "    cpu.pc = " << hex32(address) << ";\n"
                << "    bus.event_tick(cpu, false);\n";
            return;
        }
        const auto instruction = word(address);
        const auto decoded = decode_flow(instruction, address);
        out << "    cpu.pc = " << hex32(address) << ";\n";
        out << "    bus.event_tick(cpu, false);\n";
        if (decoded.flow != Flow::none) {
            // A control-flow instruction inside a delay slot is not emitted by
            // the retail compiler; fail loudly rather than drift.
            out << "    r6::unsupported_instruction(cpu, " << hex32(address)
                << ", " << hex32(instruction) << ");\n";
            return;
        }
        emit_body(out, instruction, address);
        out << "    cpu.pc += 4;\n"
            << "    bus.advance_instruction();\n";
    }

    void emit_control(std::ostream& out, std::uint32_t instruction,
                      std::uint32_t pc, const Decoded& decoded) {
        const auto opcode = instruction >> 26;
        const auto rs = reg(instruction, 21);
        const auto rt = reg(instruction, 16);
        const auto rd = reg(instruction, 11);

        if (decoded.flow == Flow::terminal) {
            out << "    r6::unsupported_instruction(cpu, " << hex32(pc) << ", "
                << hex32(instruction) << ");\n";
            return;
        }

        if (decoded.flow == Flow::call) {
            out << "    cpu.gpr[31] = " << hex32(pc + 8) << ";\n"
                << "    bus.advance_instruction();\n";
            emit_delay_slot(out, pc + 4);
            out << "    cpu.pc = " << hex32(decoded.target) << ";\n"
                << "    return cpu.pc;\n";
            return;
        }

        if (decoded.flow == Flow::jump) {
            out << "    bus.advance_instruction();\n";
            emit_delay_slot(out, pc + 4);
            out << "    cpu.pc = " << hex32(decoded.target) << ";\n"
                << "    return cpu.pc;\n";
            return;
        }

        if (decoded.flow == Flow::branch) {
            if (decoded.link) {
                out << "    cpu.gpr[31] = " << hex32(pc + 8) << ";\n";
            }
            const auto name = "taken_" + address_suffix(pc);
            if (decoded.bc2) {
                out << "    const bool " << name << " = "
                    << (decoded.bc2_taken ? "true" : "false") << ";\n";
                out << "    (void)" << name << ";\n";
            } else if (opcode == 0x01) {
                const bool less = rt == 0x00 || rt == 0x10;
                out << "    const bool " << name
                    << " = static_cast<std::int32_t>(cpu.gpr[" << rs
                    << "]) " << (less ? "<" : ">=") << " 0;\n";
            } else if (opcode == 0x04) {
                out << "    const bool " << name << " = cpu.gpr[" << rs
                    << "] == cpu.gpr[" << rt << "];\n";
            } else if (opcode == 0x05) {
                out << "    const bool " << name << " = cpu.gpr[" << rs
                    << "] != cpu.gpr[" << rt << "];\n";
            } else if (opcode == 0x06) {
                out << "    const bool " << name
                    << " = static_cast<std::int32_t>(cpu.gpr[" << rs
                    << "]) <= 0;\n";
            } else {
                out << "    const bool " << name
                    << " = static_cast<std::int32_t>(cpu.gpr[" << rs
                    << "]) > 0;\n";
            }
            out << "    bus.advance_instruction();\n";
            emit_delay_slot(out, pc + 4);
            out << "    cpu.pc = " << name << " ? " << hex32(decoded.target)
                << " : " << hex32(pc + 8) << ";\n"
                << "    return cpu.pc;\n";
            return;
        }

        // jr / jalr. Capture the target before the delay slot, as hardware
        // latches the register during the jump instruction.
        if (decoded.link && rd != 0) {
            out << "    cpu.gpr[" << rd << "] = " << hex32(pc + 8) << ";\n";
        }
        const auto target = "target_" + address_suffix(pc);
        out << "    const std::uint32_t " << target << " = cpu.gpr[" << rs
            << "];\n"
            << "    bus.advance_instruction();\n";
        emit_delay_slot(out, pc + 4);
        out << "    cpu.pc = " << target << ";\n"
            << "    return cpu.pc;\n";
    }

    void emit_body(std::ostream& out, std::uint32_t instruction,
                   std::uint32_t pc) {
        const auto opcode = instruction >> 26;
        const auto rs = reg(instruction, 21);
        const auto rt = reg(instruction, 16);
        const auto rd = reg(instruction, 11);
        const auto shamt = reg(instruction, 6);
        const auto offset = hex32(sign_extend16(immediate(instruction)));
        auto line = [&](const std::string& text) { out << "    " << text << "\n"; };
        auto store = [&](std::uint32_t index, const std::string& value) {
            if (index == 0) return;
            line("cpu.gpr[" + std::to_string(index) + "] = " + value + ";");
        };
        auto source = [&](std::uint32_t index) {
            return "cpu.gpr[" + std::to_string(index) + "]";
        };
        const auto suffix = address_suffix(pc);

        switch (opcode) {
            case 0x00: {
                switch (instruction & 0x3Fu) {
                    case 0x00: store(rd, source(rt) + " << " + std::to_string(shamt)); break;
                    case 0x02: store(rd, source(rt) + " >> " + std::to_string(shamt)); break;
                    case 0x03:
                        store(rd, "static_cast<std::uint32_t>(static_cast<std::int32_t>(" +
                                      source(rt) + ") >> " + std::to_string(shamt) + ")");
                        break;
                    case 0x04: store(rd, source(rt) + " << (" + source(rs) + " & 31u)"); break;
                    case 0x06: store(rd, source(rt) + " >> (" + source(rs) + " & 31u)"); break;
                    case 0x07:
                        store(rd, "static_cast<std::uint32_t>(static_cast<std::int32_t>(" +
                                      source(rt) + ") >> (" + source(rs) + " & 31u))");
                        break;
                    case 0x0C:
                        line("bus.syscall(cpu, cpu.gpr[4]);");
                        break;
                    case 0x10: store(rd, "cpu.hi"); break;
                    case 0x11: line("cpu.hi = " + source(rs) + ";"); break;
                    case 0x12: store(rd, "cpu.lo"); break;
                    case 0x13: line("cpu.lo = " + source(rs) + ";"); break;
                    case 0x18: {
                        const auto p = "product_" + suffix;
                        line("const std::int64_t " + p +
                             " = static_cast<std::int64_t>(static_cast<std::int32_t>(" +
                             source(rs) +
                             ")) * static_cast<std::int64_t>(static_cast<std::int32_t>(" +
                             source(rt) + "));");
                        line("cpu.lo = static_cast<std::uint32_t>(" + p + ");");
                        line("cpu.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(" +
                             p + ") >> 32);");
                        break;
                    }
                    case 0x19: {
                        const auto p = "product_" + suffix;
                        line("const std::uint64_t " + p +
                             " = static_cast<std::uint64_t>(" + source(rs) +
                             ") * static_cast<std::uint64_t>(" + source(rt) + ");");
                        line("cpu.lo = static_cast<std::uint32_t>(" + p + ");");
                        line("cpu.hi = static_cast<std::uint32_t>(" + p + " >> 32);");
                        break;
                    }
                    case 0x1A:
                        line("if (" + source(rt) + " != 0) {");
                        line("    const std::int32_t numerator_" + suffix +
                             " = static_cast<std::int32_t>(" + source(rs) + ");");
                        line("    const std::int32_t denominator_" + suffix +
                             " = static_cast<std::int32_t>(" + source(rt) + ");");
                        line("    cpu.lo = static_cast<std::uint32_t>(numerator_" +
                             suffix + " / denominator_" + suffix + ");");
                        line("    cpu.hi = static_cast<std::uint32_t>(numerator_" +
                             suffix + " % denominator_" + suffix + ");");
                        line("}");
                        break;
                    case 0x1B:
                        line("if (" + source(rt) + " != 0) {");
                        line("    cpu.lo = " + source(rs) + " / " + source(rt) + ";");
                        line("    cpu.hi = " + source(rs) + " % " + source(rt) + ";");
                        line("}");
                        break;
                    case 0x20:
                    case 0x21:
                        store(rd, source(rs) + " + " + source(rt));
                        break;
                    case 0x22:
                    case 0x23:
                        store(rd, source(rs) + " - " + source(rt));
                        break;
                    case 0x24: store(rd, source(rs) + " & " + source(rt)); break;
                    case 0x25: store(rd, source(rs) + " | " + source(rt)); break;
                    case 0x26: store(rd, source(rs) + " ^ " + source(rt)); break;
                    case 0x27: store(rd, "~(" + source(rs) + " | " + source(rt) + ")"); break;
                    case 0x2A:
                        store(rd, "static_cast<std::int32_t>(" + source(rs) +
                                      ") < static_cast<std::int32_t>(" + source(rt) +
                                      ") ? 1u : 0u");
                        break;
                    case 0x2B:
                        store(rd, source(rs) + " < " + source(rt) + " ? 1u : 0u");
                        break;
                    default:
                        line("r6::unsupported_instruction(cpu, " + hex32(pc) + ", " +
                             hex32(instruction) + ");");
                        break;
                }
                break;
            }
            case 0x08:
            case 0x09:
                store(rt, source(rs) + " + " + offset);
                break;
            case 0x0A:
                store(rt, "static_cast<std::int32_t>(" + source(rs) +
                              ") < static_cast<std::int32_t>(" + offset +
                              ") ? 1u : 0u");
                break;
            case 0x0B:
                store(rt, source(rs) + " < " + offset + " ? 1u : 0u");
                break;
            case 0x0C:
                store(rt, source(rs) + " & " + hex16(immediate(instruction)));
                break;
            case 0x0D:
                store(rt, source(rs) + " | " + hex16(immediate(instruction)));
                break;
            case 0x0E:
                store(rt, source(rs) + " ^ " + hex16(immediate(instruction)));
                break;
            case 0x0F:
                store(rt, hex32(immediate(instruction) << 16));
                break;
            case 0x10:
                if ((instruction & 0x3Fu) == 0x10) {
                    line("bus.rfe(cpu);");
                } else if (rs == 0x00) {
                    store(rt, "cpu.cop0[" + std::to_string(rd) + "]");
                } else if (rs == 0x04) {
                    line("cpu.cop0[" + std::to_string(rd) + "] = " + source(rt) + ";");
                } else {
                    line("r6::unsupported_instruction(cpu, " + hex32(pc) + ", " +
                         hex32(instruction) + ");");
                }
                break;
            case 0x12:
                if (rs == 0x00) {
                    store(rt, "bus.gte_read_data(" + std::to_string(rd) + ")");
                } else if (rs == 0x02) {
                    store(rt, "bus.gte_read_control(" + std::to_string(rd) + ")");
                } else if (rs == 0x04) {
                    line("bus.gte_write_data(" + std::to_string(rd) + ", " +
                         source(rt) + ");");
                } else if (rs == 0x06) {
                    line("bus.gte_write_control(" + std::to_string(rd) + ", " +
                         source(rt) + ");");
                } else if (rs == 0x08) {
                    // BC2F/BC2T are emitted as control flow.
                } else if (rs >= 0x10) {
                    line("bus.gte_execute(" + hex32(instruction) + ");");
                } else {
                    line("r6::unsupported_instruction(cpu, " + hex32(pc) + ", " +
                         hex32(instruction) + ");");
                }
                break;
            case 0x20:
                store(rt, "static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(bus.read8(" +
                              source(rs) + " + " + offset + "))))");
                break;
            case 0x21:
                store(rt, "static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(bus.read16(" +
                              source(rs) + " + " + offset + "))))");
                break;
            case 0x22: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                store(rt, "r6::lwl(" + source(rt) + ", bus.read32(" + a + " & ~3u), " + a + ")");
                break;
            }
            case 0x23: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                store(rt, "bus.read32(" + a + ")");
                break;
            }
            case 0x24: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                store(rt, "bus.read8(" + a + ")");
                break;
            }
            case 0x25: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                store(rt, "bus.read16(" + a + ")");
                break;
            }
            case 0x26: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                store(rt, "r6::lwr(" + source(rt) + ", bus.read32(" + a + " & ~3u), " + a + ")");
                break;
            }
            case 0x28: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.write8(" + a + ", " + source(rt) + ");");
                break;
            }
            case 0x29: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.write16(" + a + ", " + source(rt) + ");");
                break;
            }
            case 0x2A: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.write32(" + a + " & ~3u, r6::swl(" + source(rt) +
                     ", bus.read32(" + a + " & ~3u), " + a + "));");
                break;
            }
            case 0x2B: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.write32(" + a + ", " + source(rt) + ");");
                break;
            }
            case 0x2E: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.write32(" + a + " & ~3u, r6::swr(" + source(rt) +
                     ", bus.read32(" + a + " & ~3u), " + a + "));");
                break;
            }
            case 0x32: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.gte_write_data(" + std::to_string(rt) + ", bus.read32(" + a + "));");
                break;
            }
            case 0x3A: {
                const auto a = "address_" + suffix;
                line("const std::uint32_t " + a + " = " + source(rs) + " + " + offset + ";");
                line("bus.write32(" + a + ", bus.gte_read_data(" + std::to_string(rt) + "));");
                break;
            }
            default:
                line("r6::unsupported_instruction(cpu, " + hex32(pc) + ", " +
                     hex32(instruction) + ");");
                break;
        }
    }

    static std::string address_suffix(std::uint32_t pc) {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%08X", pc);
        return buffer;
    }

    Image image_;
    std::string name_;
    std::set<std::uint32_t> leaders_;
    std::set<std::uint32_t> delay_slots_;
    std::vector<std::uint32_t> queue_;
};

std::vector<std::uint32_t> read_extra_leaders(const std::string& path) {
    std::vector<std::uint32_t> extra;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream stream(line);
        std::string token;
        while (stream >> token) {
            try {
                extra.push_back(static_cast<std::uint32_t>(
                    std::stoul(token, nullptr, 0)));
            } catch (...) {
            }
        }
    }
    return extra;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: r6-recomp <image.chd|image.cue|image.bin|SLUS_xxx>"
                     " <output.cpp> [--name NAME] [--extra-leaders FILE]\n";
        return 1;
    }
    const std::filesystem::path input = argv[1];
    const std::filesystem::path output = argv[2];
    std::string name = "game";
    std::vector<std::uint32_t> extra;
    bool seed_pointers = true;

    for (int index = 3; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--name" && index + 1 < argc) {
            name = argv[++index];
        } else if (argument == "--extra-leaders" && index + 1 < argc) {
            extra = read_extra_leaders(argv[++index]);
        } else if (argument == "--no-seed-pointers") {
            seed_pointers = false;
        } else {
            std::cerr << "r6-recomp: unknown argument " << argument << "\n";
            return 1;
        }
    }

    Image image;
    std::string error;
    if (!load_image(input, image, error)) {
        std::cerr << "r6-recomp: " << error << "\n";
        return 2;
    }

    std::error_code code;
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), code);
    }
    if (code) {
        std::cerr << "r6-recomp: cannot create " << output.parent_path().string()
                  << "\n";
        return 3;
    }

    Recompiler recompiler(image, name);
    recompiler.discover(extra, seed_pointers);

    std::ofstream source(output);
    if (!source) {
        std::cerr << "r6-recomp: cannot write " << output.string() << "\n";
        return 4;
    }
    recompiler.emit(source);
    std::cout << "wrote " << output.string() << "\n";
    return 0;
}
