#include "r6/gte.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace r6 {

namespace {

using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u32 = std::uint32_t;

// A MAC result that no longer fits in 43 bits sets an overflow flag; the stored
// accumulator is 32 bits wide.
constexpr s64 kMacLimit = (s64{1} << 43) - 1;
constexpr s64 kMacNegativeLimit = -(s64{1} << 43);

// Bits 30-23 and 18-13: what FLAG bit 31 reports.
constexpr u32 kFlagPayloadMask = 0x7F87E000u;

constexpr s32 kScreenMinimum = -0x400;
constexpr s32 kScreenMaximum = 0x3FF;

constexpr s64 kFixedOne = 0x1000;  // One in 3.12 fixed point.

s16 low_half(u32 value) { return static_cast<s16>(value & 0xFFFFu); }

s16 high_half(u32 value) { return static_cast<s16>(value >> 16); }

u32 signed_word(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

// IR1..IR3 and MAC1..MAC3 saturate flags are numbered downwards from IR1/MAC1.
u32 ir_flag(std::size_t index) {
    return Gte::kFlagIr1Saturated >> (index - 1);
}

u32 mac_positive_flag(std::size_t index) {
    return Gte::kFlagMac1Positive >> (index - 1);
}

u32 mac_negative_flag(std::size_t index) {
    return Gte::kFlagMac1Negative >> (index - 1);
}

}  // namespace

void Gte::reset() {
    data_.fill(0);
    control_.fill(0);
    unsupported_by_opcode_.fill(0);
    executed_ = 0;
    unsupported_ = 0;
    last_unsupported_ = 0;
}

std::uint32_t Gte::read_data(std::uint32_t reg) const {
    const auto index = reg & 31u;
    switch (index) {
        // Whole-register 16-bit values are sign-extended on read.
        case kVz0:
        case kVz1:
        case kVz2:
        case kIr0:
        case kIr1:
        case kIr2:
        case kIr3:
            return signed_word(low_half(data_[index]));
        // Screen and ordering-table Z are unsigned.
        case kOtz:
        case kSz0:
        case kSz1:
        case kSz2:
        case kSz3:
            return data_[index] & 0xFFFFu;
        // SXYP is a mirror of SXY2 with move-on-write, not a register of its
        // own (see write_data).
        case kSxyp:
            return data_[kSxy2];
        // Both colour-conversion registers read back the same derivation from
        // IR1..IR3.
        case kIrgb:
        case kOrgb:
            return color_15();
        case kLzcr:
            return leading_bits();
        default:
            return data_[index];
    }
}

void Gte::write_data(std::uint32_t reg, std::uint32_t value) {
    const auto index = reg & 31u;
    switch (index) {
        case kSxyp:
            // Writing SXYP shifts the FIFO down, exactly like a command that
            // writes SXY2 directly.
            data_[kSxy0] = data_[kSxy1];
            data_[kSxy1] = data_[kSxy2];
            data_[kSxy2] = value;
            return;
        case kIrgb:
            // Expands 5:5:5 to 16-bit IR channels, scaled by 80h.
            data_[kIr1] = signed_word(
                static_cast<s16>((value & 0x1Fu) * 0x80u));
            data_[kIr2] = signed_word(
                static_cast<s16>(((value >> 5) & 0x1Fu) * 0x80u));
            data_[kIr3] = signed_word(
                static_cast<s16>(((value >> 10) & 0x1Fu) * 0x80u));
            return;
        default:
            data_[index] = value;
            return;
    }
}

std::uint32_t Gte::read_control(std::uint32_t reg) const {
    const auto index = reg & 31u;
    switch (index) {
        // The 16-bit control registers read back sign-extended, including the
        // documented sign-extension bug on the unsigned H register.
        case kProjectionH:
        case kDepthA:
        case kZsf3:
        case kZsf4:
        case 4:   // RT33
        case 12:  // L33
        case 20:  // LB3
            return signed_word(low_half(control_[index]));
        default:
            return control_[index];
    }
}

void Gte::write_control(std::uint32_t reg, std::uint32_t value) {
    const auto index = reg & 31u;
    // Register writes are not saturated by the hardware; H is simply 16 bits.
    if (index == kProjectionH) {
        control_[index] = value & 0xFFFFu;
        return;
    }
    control_[index] = value;
}

bool Gte::execute(std::uint32_t instruction) {
    // Every command clears the FLAG payload before it runs. Software may set
    // those bits, but they do not survive a command.
    control_[kFlag] = 0;
    const bool sf = (instruction & (1u << 19)) != 0;
    const bool lm = (instruction & (1u << 10)) != 0;

    bool implemented = true;
    switch (instruction & 0x3Fu) {
        case kRtpsOpcode:
            perspective(false, sf, lm);
            break;
        case kRtptOpcode:
            perspective(true, sf, lm);
            break;
        case kNclipOpcode:
            normal_clip();
            break;
        case kAvsz3Opcode:
            average_z(false);
            break;
        case kAvsz4Opcode:
            average_z(true);
            break;
        case kMvmvaOpcode:
            matrix_vector(instruction, sf, lm);
            break;
        case kNcsOpcode:
            color_light(Lighting::ir_only, false, sf, lm);
            break;
        case kNctOpcode:
            color_light(Lighting::ir_only, true, sf, lm);
            break;
        case kNccsOpcode:
            color_light(Lighting::primary, false, sf, lm);
            break;
        case kNcctOpcode:
            color_light(Lighting::primary, true, sf, lm);
            break;
        case kNcdsOpcode:
            color_light(Lighting::depth_cue, false, sf, lm);
            break;
        case kNcdtOpcode:
            color_light(Lighting::depth_cue, true, sf, lm);
            break;
        case kSqrOpcode:
            square(sf);
            break;
        default:
            implemented = false;
            ++unsupported_;
            last_unsupported_ = instruction & 0x3Fu;
            ++unsupported_by_opcode_[last_unsupported_];
            break;
    }
    if (implemented) ++executed_;
    finish_command();
    return implemented;
}

void Gte::perspective(bool triple, bool sf, bool lm) {
    const unsigned vertices = triple ? 3u : 1u;
    const unsigned shift = sf ? 12u : 0u;

    for (unsigned vertex = 0; vertex < vertices; ++vertex) {
        const auto vxy = data_[kVxy0 + vertex * 2u];
        const s64 vx = low_half(vxy);
        const s64 vy = high_half(vxy);
        const s64 vz = low_half(data_[kVz0 + vertex * 2u]);

        // MAC1..MAC3 = (TR*1000h + RT*V) SAR (sf*12). Signed right shift is
        // arithmetic in C++20, which is what SAR means here.
        s64 mac[3];
        for (std::size_t row = 0; row < 3; ++row) {
            const s64 translation =
                static_cast<s32>(control_[kTranslationX + row]);
            const s64 product =
                translation * kFixedOne +
                static_cast<s64>(matrix(kRotation, row * 3 + 0)) * vx +
                static_cast<s64>(matrix(kRotation, row * 3 + 1)) * vy +
                static_cast<s64>(matrix(kRotation, row * 3 + 2)) * vz;
            mac[row] = checked_mac(product, mac_positive_flag(row + 1),
                                   mac_negative_flag(row + 1)) >>
                       shift;
        }
        store_mac(1, static_cast<s32>(mac[0]));
        store_mac(2, static_cast<s32>(mac[1]));
        store_mac(3, static_cast<s32>(mac[2]));

        // IR3's saturation flag always behaves as if lm=0, while its stored
        // value still honours lm.
        const s16 ir1 = clamp_ir(mac[0], 1, lm, false);
        const s16 ir2 = clamp_ir(mac[1], 2, lm, false);
        const s16 ir3 = clamp_ir(mac[2], 3, lm, true);
        store_ir(1, ir1);
        store_ir(2, ir2);
        store_ir(3, ir3);

        // SZ3 = MAC3 SAR ((1-sf)*12), saturated to 0..FFFFh.
        push_screen_z(clamp_z(mac[2] >> (sf ? 0u : 12u)));

        // Perspective division. H >= SZ3*2 means the vertex is at or beyond the
        // near plane, so the result saturates and the overflow flag is set.
        const s64 h = control_[kProjectionH] & 0xFFFFu;
        const s64 sz3 = data_[kSz3] & 0xFFFFu;
        s64 scale = 0x1FFFF;
        if (sz3 == 0 || h >= sz3 * 2) {
            set_flag(kFlagDivideOverflow);
        } else {
            scale = ((h * 0x20000 / sz3) + 1) / 2;
            if (scale > 0x1FFFF) {
                set_flag(kFlagDivideOverflow);
                scale = 0x1FFFF;
            }
        }

        const s32 ofx = static_cast<s32>(control_[kOffsetX]);
        const s32 ofy = static_cast<s32>(control_[kOffsetY]);
        const s64 dqa = low_half(control_[kDepthA]);
        const s64 dqb = static_cast<s32>(control_[kDepthB]);

        const s32 mac0_x = clamp_mac0(scale * ir1 + ofx);
        const auto sx = clamp_screen(mac0_x >> 16, kFlagSx2Saturated);
        const s32 mac0_y = clamp_mac0(scale * ir2 + ofy);
        const auto sy = clamp_screen(mac0_y >> 16, kFlagSy2Saturated);
        push_screen(sx, sy);

        // Depth cueing: IR0 = MAC0/1000h, saturated to 0..1000h.
        const s32 mac0_depth = clamp_mac0(scale * dqa + dqb);
        store_mac(0, mac0_depth);
        s64 ir0 = mac0_depth >> 12;
        if (ir0 < 0 || ir0 > 0x1000) {
            set_flag(kFlagIr0Saturated);
            ir0 = std::clamp<s64>(ir0, 0, 0x1000);
        }
        store_ir(0, static_cast<s16>(ir0));
    }
}

void Gte::normal_clip() {
    const s16 sx0 = low_half(data_[kSxy0]);
    const s16 sy0 = high_half(data_[kSxy0]);
    const s16 sx1 = low_half(data_[kSxy1]);
    const s16 sy1 = high_half(data_[kSxy1]);
    const s16 sx2 = low_half(data_[kSxy2]);
    const s16 sy2 = high_half(data_[kSxy2]);

    const s64 value = static_cast<s64>(sx0) * sy1 +
                      static_cast<s64>(sx1) * sy2 +
                      static_cast<s64>(sx2) * sy0 -
                      static_cast<s64>(sx0) * sy2 -
                      static_cast<s64>(sx1) * sy0 -
                      static_cast<s64>(sx2) * sy1;
    store_mac(0, clamp_mac0(value));
}

void Gte::average_z(bool four) {
    const auto z = [this](u32 reg) {
        return static_cast<s64>(data_[reg] & 0xFFFFu);
    };
    const s64 sum = four ? z(kSz0) + z(kSz1) + z(kSz2) + z(kSz3)
                         : z(kSz1) + z(kSz2) + z(kSz3);
    const s64 factor = low_half(control_[four ? kZsf4 : kZsf3]);
    const s64 mac0 = factor * sum;

    store_mac(0, clamp_mac0(mac0));
    data_[kOtz] = clamp_z(mac0 >> 12);
}

std::array<std::int64_t, 3> Gte::load_vector(std::size_t index) const {
    const auto vxy = data_[kVxy0 + index * 2];
    return {low_half(vxy), high_half(vxy),
            low_half(data_[kVz0 + index * 2])};
}

void Gte::matrix_vector(std::uint32_t instruction, bool sf, bool lm) {
    const u32 matrix_select = (instruction >> 17) & 3u;
    const u32 vector_select = (instruction >> 15) & 3u;
    const u32 translation_select = (instruction >> 13) & 3u;

    std::uint32_t base = kRotation;
    if (matrix_select == 1) base = kLightMatrix;
    if (matrix_select == 2) base = kColorMatrix;
    // Selector 3 is reserved: hardware returns a garbage matrix. The rotation
    // matrix is the least misleading stand-in until something needs it.

    const std::array<s64, 3> vector =
        vector_select == 3 ? std::array<s64, 3>{ir_value(1), ir_value(2),
                                               ir_value(3)}
                           : load_vector(vector_select);

    std::array<s64, 3> translation{};
    if (translation_select == 0) {
        translation = {static_cast<s32>(control_[kTranslationX]),
                       static_cast<s32>(control_[kTranslationY]),
                       static_cast<s32>(control_[kTranslationZ])};
    } else if (translation_select == 1) {
        translation = {static_cast<s32>(control_[kBackgroundR]),
                       static_cast<s32>(control_[kBackgroundG]),
                       static_cast<s32>(control_[kBackgroundB])};
    }

    matrix_product(base, vector, translation,
                   translation_select == 2 ? MatrixSum::last_two_terms
                                           : MatrixSum::all_terms,
                   sf, lm);
}

void Gte::matrix_product(std::size_t base,
                         const std::array<std::int64_t, 3>& vector,
                         const std::array<std::int64_t, 3>& translation,
                         MatrixSum sum, bool sf, bool lm) {
    const unsigned shift = sf ? 12u : 0u;
    for (std::size_t row = 0; row < 3; ++row) {
        s64 value = 0;
        if (sum == MatrixSum::all_terms) {
            value = translation[row] * kFixedOne +
                    static_cast<s64>(matrix(base, row * 3 + 0)) * vector[0];
        }
        value += static_cast<s64>(matrix(base, row * 3 + 1)) * vector[1];
        value += static_cast<s64>(matrix(base, row * 3 + 2)) * vector[2];
        value = checked_mac(value, mac_positive_flag(row + 1),
                            mac_negative_flag(row + 1)) >>
                shift;
        store_mac(row + 1, static_cast<s32>(value));
        store_ir(row + 1, clamp_ir(value, row + 1, lm, false));
    }
}

void Gte::color_light(Lighting mode, bool triple, bool sf, bool lm) {
    const std::array<s64, 3> no_translation{};
    const std::array<s64, 3> background{
        static_cast<s32>(control_[kBackgroundR]),
        static_cast<s32>(control_[kBackgroundG]),
        static_cast<s32>(control_[kBackgroundB]),
    };
    const auto rgbc = data_[kRgbc];
    const std::array<s64, 3> primary{static_cast<s64>(rgbc & 0xFFu),
                                     static_cast<s64>((rgbc >> 8) & 0xFFu),
                                     static_cast<s64>((rgbc >> 16) & 0xFFu)};
    const unsigned shift = sf ? 12u : 0u;
    const bool depth_cue = mode == Lighting::depth_cue;

    const std::size_t vertices = triple ? 3u : 1u;
    for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
        // The normal arrives through the light matrix, then the resulting
        // intensities through the colour matrix with the background colour.
        matrix_product(kLightMatrix, load_vector(vertex), no_translation,
                       MatrixSum::all_terms, sf, lm);
        const std::array<s64, 3> lit{ir_value(1), ir_value(2), ir_value(3)};
        matrix_product(kColorMatrix, lit, background, MatrixSum::all_terms, sf,
                       lm);

        // NCS/NCT stop here. They exist to produce IR for a following CC/CDP
        // and leave the colour FIFO untouched, so a triple run ends holding the
        // last vertex's IR.
        if (mode == Lighting::ir_only) continue;

        for (std::size_t index = 0; index < 3; ++index) {
            // MAC = (R*IR1, G*IR2, B*IR3) SHL 4.
            s64 value = primary[index] * ir_value(index + 1) * 16;

            if (depth_cue) {
                // IR = ((FC SHL 12) - MAC) SAR (sf*12), saturated as if lm=0,
                // then MAC = IR * IR0 + MAC. Only the final MAC is
                // range-checked: the difference is a working value that is
                // immediately consumed.
                const auto far_source =
                    static_cast<s32>(control_[kFarColorR + index]);
                const s64 difference =
                    (static_cast<s64>(far_source) * kFixedOne - value) >> shift;
                const auto step = clamp_ir(difference, index + 1, false, false);
                store_ir(index + 1, step);
                value += static_cast<s64>(step) * ir_value(0);
            }

            value = checked_mac(value, mac_positive_flag(index + 1),
                                mac_negative_flag(index + 1)) >>
                    shift;
            store_mac(index + 1, static_cast<s32>(value));
            store_ir(index + 1, clamp_ir(value, index + 1, lm, false));
        }
        push_color();
    }
}

void Gte::push_color() {
    u32 rgb = 0;
    for (std::size_t index = 0; index < 3; ++index) {
        s64 value = static_cast<s32>(data_[kMac1 + index]) >> 4;
        if (value < 0) {
            set_flag(kFlagColorRSaturated >> index);
            value = 0;
        } else if (value > 0xFF) {
            set_flag(kFlagColorRSaturated >> index);
            value = 0xFF;
        }
        rgb |= static_cast<u32>(value) << (index * 8);
    }
    // The top byte of RGBC is the rendering command a caller packs in; it rides
    // along untouched in the FIFO entry's code byte.
    const u32 code = data_[kRgbc] & 0xFF000000u;
    data_[kRgb0] = data_[kRgb1];
    data_[kRgb1] = data_[kRgb2];
    data_[kRgb2] = code | rgb;
}

void Gte::square(bool sf) {
    const unsigned shift = sf ? 12u : 0u;
    for (std::size_t index = 1; index <= 3; ++index) {
        const s64 ir = ir_value(index);
        const s64 value = checked_mac(ir * ir, mac_positive_flag(index),
                                      mac_negative_flag(index)) >>
                          shift;
        store_mac(index, static_cast<s32>(value));
        // A square is never negative, so the hardware applies the lm range
        // regardless of the bit: 0..7FFFh.
        store_ir(index, clamp_ir(value, index, true, false));
    }
}

const char* Gte::opcode_name(std::uint32_t opcode) {
    switch (opcode & 0x3Fu) {
        case 0x01: return "RTPS";
        case 0x06: return "NCLIP";
        case 0x0C: return "OP";
        case 0x10: return "DPCS";
        case 0x11: return "INTPL";
        case 0x12: return "MVMVA";
        case 0x13: return "NCDS";
        case 0x14: return "CDP";
        case 0x16: return "NCDT";
        case 0x1B: return "NCCS";
        case 0x1C: return "CC";
        case 0x1E: return "NCS";
        case 0x20: return "NCT";
        case 0x28: return "SQR";
        case 0x29: return "DCPL";
        case 0x2A: return "DPCT";
        case 0x2D: return "AVSZ3";
        case 0x2E: return "AVSZ4";
        case 0x30: return "RTPT";
        case 0x3D: return "GPF";
        case 0x3E: return "GPL";
        case 0x3F: return "NCCT";
        default:
            // Reserved encodings have no name on hardware either.
            return nullptr;
    }
}

std::string Gte::unsupported_summary(std::size_t limit) const {
    std::vector<u32> order;
    for (u32 opcode = 0; opcode < unsupported_by_opcode_.size(); ++opcode) {
        if (unsupported_by_opcode_[opcode] != 0) order.push_back(opcode);
    }
    if (order.empty() || limit == 0) return {};

    std::sort(order.begin(), order.end(), [this](u32 left, u32 right) {
        if (unsupported_by_opcode_[left] != unsupported_by_opcode_[right]) {
            return unsupported_by_opcode_[left] > unsupported_by_opcode_[right];
        }
        // Stable ordering so two runs with equal counts read the same.
        return left < right;
    });

    std::ostringstream text;
    text << " (";
    for (std::size_t index = 0; index < order.size() && index < limit;
         ++index) {
        if (index != 0) text << ", ";
        const auto opcode = order[index];
        text << "0x" << std::hex << opcode << std::dec;
        if (const char* name = opcode_name(opcode)) text << " " << name;
        text << " x" << unsupported_by_opcode_[opcode];
    }
    if (order.size() > limit) text << ", +" << order.size() - limit << " more";
    text << ")";
    return text.str();
}

std::int64_t Gte::checked_mac(s64 value, u32 positive_flag,
                              u32 negative_flag) {
    if (value > kMacLimit) {
        set_flag(positive_flag);
        return kMacLimit;
    }
    if (value < kMacNegativeLimit) {
        set_flag(negative_flag);
        return kMacNegativeLimit;
    }
    return value;
}

std::int32_t Gte::clamp_mac0(s64 value) {
    if (value > 0x7FFFFFFF) {
        set_flag(kFlagMac0Positive);
        return 0x7FFFFFFF;
    }
    if (value < -0x80000000LL) {
        set_flag(kFlagMac0Negative);
        return -0x7FFFFFFF - 1;
    }
    return static_cast<s32>(value);
}

std::int16_t Gte::clamp_ir(s64 value, std::size_t index, bool lm,
                           bool flag_as_lm0) {
    const s64 flag_minimum = (lm && !flag_as_lm0) ? 0 : -0x8000;
    const s64 stored_minimum = lm ? 0 : -0x8000;
    if (value < flag_minimum || value > 0x7FFF) set_flag(ir_flag(index));
    return static_cast<s16>(
        std::clamp<s64>(value, stored_minimum, 0x7FFF));
}

std::uint32_t Gte::clamp_z(s64 value) {
    if (value < 0) {
        set_flag(kFlagSzSaturated);
        return 0;
    }
    if (value > 0xFFFF) {
        set_flag(kFlagSzSaturated);
        return 0xFFFFu;
    }
    return static_cast<std::uint32_t>(value);
}

std::int32_t Gte::clamp_screen(s32 value, std::uint32_t flag) {
    if (value < kScreenMinimum) {
        set_flag(flag);
        return kScreenMinimum;
    }
    if (value > kScreenMaximum) {
        set_flag(flag);
        return kScreenMaximum;
    }
    return value;
}

void Gte::push_screen(s32 x, s32 y) {
    data_[kSxy0] = data_[kSxy1];
    data_[kSxy1] = data_[kSxy2];
    data_[kSxy2] = pack_screen(x, y);
}

void Gte::push_screen_z(u32 z) {
    data_[kSz0] = data_[kSz1];
    data_[kSz1] = data_[kSz2];
    data_[kSz2] = data_[kSz3];
    data_[kSz3] = z & 0xFFFFu;
}

std::uint32_t Gte::pack_screen(s32 x, s32 y) {
    return (static_cast<u32>(x) & 0xFFFFu) |
           (static_cast<u32>(y) << 16);
}

void Gte::finish_command() {
    // Bit 31 is the "any calculation error" summary of bits 30-23 and 18-13.
    // Note that the summary deliberately leaves out bit 22 (IR3) and the colour
    // saturation bits, so it is derived from the mask rather than from the
    // whole register. Every other flag is kept as set.
    const bool error = (control_[kFlag] & kFlagPayloadMask) != 0;
    control_[kFlag] = (control_[kFlag] & ~kFlagError) |
                      (error ? kFlagError : 0u);
}

void Gte::store_mac(std::size_t index, s32 value) {
    data_[kMac0 + index] = static_cast<u32>(value);
}

void Gte::store_ir(std::size_t index, s16 value) {
    data_[kIr0 + index] = static_cast<u32>(static_cast<s32>(value));
}

std::int16_t Gte::ir_value(std::size_t index) const {
    return low_half(data_[kIr0 + index]);
}

std::int16_t Gte::matrix(std::size_t base, std::size_t element) const {
    const auto word = control_[base + element / 2];
    return (element % 2 == 0) ? low_half(word) : high_half(word);
}

std::uint32_t Gte::color_15() const {
    const auto channel = [](s16 value) {
        if (value < 0) return 0u;
        const auto scaled = static_cast<u32>(value) / 0x80u;
        return std::min(scaled, 0x1Fu);
    };
    return channel(ir_value(1)) | (channel(ir_value(2)) << 5) |
           (channel(ir_value(3)) << 10);
}

std::uint32_t Gte::leading_bits() const {
    const auto value = data_[kLzcs];
    // Leading zeroes of a positive value, leading ones of a negative one.
    u32 probe = (value & 0x80000000u) != 0 ? ~value : value;
    u32 count = 0;
    while (count < 32 && (probe & 0x80000000u) == 0) {
        probe <<= 1;
        ++count;
    }
    return count;
}

}  // namespace r6
