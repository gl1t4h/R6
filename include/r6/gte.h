#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace r6 {

// PlayStation Geometry Transformation Engine, reached through COP2.
//
// Registers and command semantics follow psx-spx, "Geometry Transformation
// Engine (GTE)". Commands arrive in the COP2 immediate form: bit 25 set, the
// real opcode in bits 0-5, "sf" (shift fraction) in bit 19, "lm" (IR saturation
// range) in bit 10, and for MVMVA the matrix/vector/translation selectors in
// bits 17-18, 15-16 and 13-14.
//
// Implemented: RTPS, RTPT, NCLIP, AVSZ3, AVSZ4, MVMVA and SQR cover the
// coordinate path a renderer needs before it can submit a scene: transform,
// perspective divide, screen clip and ordering-table depth. The lighting path
// is in as well: NCS, NCCS and NCDS turn a vertex normal into IR values and,
// for the latter two, a colour-FIFO entry, and NCT, NCCT and NCDT run the same
// chain for V0, V1 and V2.
//
// Not implemented: the colour commands that work from existing IR or FIFO
// values (CC, CDP, DCPL, DPCS, DPCT, INTPL), plus OP, GPF and GPL. Those
// opcodes are accepted, counted and reported per opcode, but compute nothing
// yet; geometry does not depend on them.
class Gte {
public:
    using u32 = std::uint32_t;
    using s16 = std::int16_t;
    using s32 = std::int32_t;

    // cop2r0-31, through mfc2/mtc2 and lwc2/swc2.
    enum DataRegister : u32 {
        kVxy0 = 0, kVz0 = 1, kVxy1 = 2, kVz1 = 3, kVxy2 = 4, kVz2 = 5,
        kRgbc = 6, kOtz = 7, kIr0 = 8, kIr1 = 9, kIr2 = 10, kIr3 = 11,
        kSxy0 = 12, kSxy1 = 13, kSxy2 = 14, kSxyp = 15,
        kSz0 = 16, kSz1 = 17, kSz2 = 18, kSz3 = 19,
        kRgb0 = 20, kRgb1 = 21, kRgb2 = 22, kRes1 = 23,
        kMac0 = 24, kMac1 = 25, kMac2 = 26, kMac3 = 27,
        kIrgb = 28, kOrgb = 29, kLzcs = 30, kLzcr = 31,
    };

    // cop2r32-63, through cfc2/ctc2.
    enum ControlRegister : u32 {
        kRotation = 0,
        kTranslationX = 5, kTranslationY = 6, kTranslationZ = 7,
        kLightMatrix = 8,
        kBackgroundR = 13, kBackgroundG = 14, kBackgroundB = 15,
        kColorMatrix = 16,
        kFarColorR = 21, kFarColorG = 22, kFarColorB = 23,
        kOffsetX = 24, kOffsetY = 25, kProjectionH = 26,
        kDepthA = 27, kDepthB = 28,
        kZsf3 = 29, kZsf4 = 30, kFlag = 31,
    };

    // FLAG bits ("GTE Saturation"). Bit 31 is derived at the end of a command.
    enum Flag : u32 {
        kFlagIr0Saturated = 1u << 12,
        kFlagSy2Saturated = 1u << 13,
        kFlagSx2Saturated = 1u << 14,
        kFlagMac0Negative = 1u << 15,
        kFlagMac0Positive = 1u << 16,
        kFlagDivideOverflow = 1u << 17,
        kFlagSzSaturated = 1u << 18,
        kFlagColorBSaturated = 1u << 19,
        kFlagColorGSaturated = 1u << 20,
        kFlagColorRSaturated = 1u << 21,
        kFlagIr3Saturated = 1u << 22,
        kFlagIr2Saturated = 1u << 23,
        kFlagIr1Saturated = 1u << 24,
        kFlagMac3Negative = 1u << 25,
        kFlagMac2Negative = 1u << 26,
        kFlagMac1Negative = 1u << 27,
        kFlagMac3Positive = 1u << 28,
        kFlagMac2Positive = 1u << 29,
        kFlagMac1Positive = 1u << 30,
        kFlagError = 1u << 31,
    };

    void reset();

    // Reads apply the documented sign extension: 16-bit registers come back
    // sign-extended, while the FIFOs and MAC registers are returned as stored.
    // IRGB/ORGB, LZCR and the SXYP mirror are computed, not stored.
    u32 read_data(u32 reg) const;
    void write_data(u32 reg, u32 value);

    // Control-register writes are stored as-is; the hardware does not saturate
    // register writes or raise FLAG bits for them.
    u32 read_control(u32 reg) const;
    void write_control(u32 reg, u32 value);

    // Runs one COP2 command. Returns false for an opcode this engine does not
    // implement, so the caller can report it instead of silently doing nothing.
    bool execute(u32 instruction);

    u32 flags() const { return control_[kFlag]; }
    std::uint64_t executed() const { return executed_; }
    std::uint64_t unsupported() const { return unsupported_; }
    u32 last_unsupported() const { return last_unsupported_; }

    // Per-opcode totals for commands this engine does not implement, so a run
    // can name the command that is missing instead of only counting it.
    const std::array<std::uint64_t, 64>& unsupported_by_opcode() const {
        return unsupported_by_opcode_;
    }
    // " (0x1e NCS x1200, ...)" for the busiest entries, empty when nothing
    // was skipped. limit caps the entries listed; the rest become "+N more".
    std::string unsupported_summary(std::size_t limit = 4) const;
    // Real opcode name (bits 0-5), or nullptr for reserved or unknown numbers.
    static const char* opcode_name(std::uint32_t opcode);

private:
    // Command opcodes, real opcode bits 0-5.
    enum Command : u32 {
        kRtpsOpcode = 0x01,
        kNclipOpcode = 0x06,
        kMvmvaOpcode = 0x12,
        kNcdsOpcode = 0x13,
        kNcdtOpcode = 0x16,
        kNccsOpcode = 0x1B,
        kNcsOpcode = 0x1E,
        kNctOpcode = 0x20,
        kNcctOpcode = 0x3F,
        kSqrOpcode = 0x28,
        kAvsz3Opcode = 0x2D,
        kAvsz4Opcode = 0x2E,
        kRtptOpcode = 0x30,
    };

    void perspective(bool triple, bool sf, bool lm);
    void normal_clip();
    void average_z(bool four);
    void matrix_vector(u32 instruction, bool sf, bool lm);
    void square(bool sf);

    // MVMVA's translation selector 2 is a documented hardware bug: each row is
    // reduced to its last two terms. Everything else sums all three plus the
    // translation.
    enum class MatrixSum { all_terms, last_two_terms };
    // NCS produces IR only, NCCS also writes the primary colour, NCDS also
    // interpolates towards the far colour.
    enum class Lighting { ir_only, primary, depth_cue };

    // Shared row loop for MVMVA and the lighting commands: MAC and IR for
    // matrix x vector, plus the translation term.
    void matrix_product(std::size_t base,
                        const std::array<std::int64_t, 3>& vector,
                        const std::array<std::int64_t, 3>& translation,
                        MatrixSum sum, bool sf, bool lm);
    // V0, V1 or V2 as (x, y, z).
    std::array<std::int64_t, 3> load_vector(std::size_t index) const;
    // Runs the chain for V0, or for V0, V1 and V2 when triple is set.
    void color_light(Lighting mode, bool triple, bool sf, bool lm);
    void push_color();

    void set_flag(u32 bit) { control_[kFlag] |= bit; }
    void finish_command();
    void store_mac(std::size_t index, s32 value);
    void store_ir(std::size_t index, s16 value);
    s16 ir_value(std::size_t index) const;

    // Saturation helpers, after psx-spx "GTE Saturation". The accumulators are
    // 44 bits wide internally, so the inputs are 64-bit.
    std::int64_t checked_mac(std::int64_t value, u32 positive_flag,
                             u32 negative_flag);
    s32 clamp_mac0(std::int64_t value);
    s16 clamp_ir(std::int64_t value, std::size_t index, bool lm,
                 bool flag_as_lm0);
    u32 clamp_z(std::int64_t value);
    s32 clamp_screen(s32 value, u32 flag);

    void push_screen(s32 x, s32 y);
    void push_screen_z(u32 z);
    static u32 pack_screen(s32 x, s32 y);

    // Element of the 3x3 matrix that starts at a control register, packed two
    // signed halves per register.
    s16 matrix(std::size_t base, std::size_t element) const;
    u32 color_15() const;
    u32 leading_bits() const;

    std::array<u32, 32> data_{};
    std::array<u32, 32> control_{};
    std::array<std::uint64_t, 64> unsupported_by_opcode_{};
    std::uint64_t executed_{0};
    std::uint64_t unsupported_{0};
    u32 last_unsupported_{0};
};

}  // namespace r6
