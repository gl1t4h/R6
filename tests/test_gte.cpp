#include "test_support.h"

#include <cstdint>
#include <string>

#include "r6/gte.h"
#include "r6/memory.h"
#include "r6/mips_interpreter.h"
#include "r6/platform_services.h"

namespace {

using r6::Gte;

// Command words as the devkit writes them (COP2 immediate, bit 25 set).
constexpr std::uint32_t kRtps = 0x0180001u;
constexpr std::uint32_t kRtpt = 0x0280030u;
constexpr std::uint32_t kNclip = 0x1400006u;
constexpr std::uint32_t kAvsz3 = 0x158002Du;
constexpr std::uint32_t kAvsz4 = 0x168002Eu;
constexpr std::uint32_t kMvmva = 0x0400012u;
constexpr std::uint32_t kSqr = 0x0A00428u;
constexpr std::uint32_t kNcs = 0x0C8041Eu;
constexpr std::uint32_t kNccs = 0x108041Bu;
constexpr std::uint32_t kNcds = 0x0E80413u;
constexpr std::uint32_t kNct = 0x0D80420u;
constexpr std::uint32_t kNcct = 0x118043Fu;
constexpr std::uint32_t kNcdt = 0x0F80416u;

std::uint32_t with_shift(std::uint32_t command, bool sf) {
    return (command & ~(1u << 19)) | (sf ? (1u << 19) : 0u);
}

std::uint32_t pack_pair(std::int32_t first, std::int32_t second) {
    return (static_cast<std::uint32_t>(first) & 0xFFFFu) |
           (static_cast<std::uint32_t>(second) << 16);
}

void identity_rotation(Gte& gte) {
    gte.write_control(Gte::kRotation, 0x1000u);      // RT11, RT12
    gte.write_control(Gte::kRotation + 2, 0x1000u);  // RT22, RT23
    gte.write_control(Gte::kRotation + 4, 0x1000u);  // RT33
}

// Identity light and colour matrices, which is the neutral lighting case.
void identity_lighting(Gte& gte) {
    gte.write_control(Gte::kLightMatrix, 0x1000u);       // L11, L12
    gte.write_control(Gte::kLightMatrix + 2, 0x1000u);   // L22, L23
    gte.write_control(Gte::kLightMatrix + 4, 0x1000u);   // L33
    gte.write_control(Gte::kColorMatrix, 0x1000u);       // LR1, LR2
    gte.write_control(Gte::kColorMatrix + 2, 0x1000u);   // LG2, LG3
    gte.write_control(Gte::kColorMatrix + 4, 0x1000u);   // LB3
}

void set_vector(Gte& gte, std::uint32_t index, std::int32_t x,
                std::int32_t y, std::int32_t z) {
    const auto base = Gte::kVxy0 + index * 2;
    gte.write_data(base, pack_pair(x, y));
    gte.write_data(base + 1, static_cast<std::uint32_t>(z));
}

std::int16_t screen_x(const Gte& gte, std::uint32_t reg) {
    return static_cast<std::int16_t>(gte.read_data(reg) & 0xFFFFu);
}

std::int16_t screen_y(const Gte& gte, std::uint32_t reg) {
    return static_cast<std::int16_t>(gte.read_data(reg) >> 16);
}

std::int32_t mac0(const Gte& gte) {
    return static_cast<std::int32_t>(gte.read_data(Gte::kMac0));
}

// A vertex on the view axis at distance z, with an identity rotation and no
// translation, must land on the screen offset with SZ3 = z.
void test_rtps_identity() {
    Gte gte;
    identity_rotation(gte);
    set_vector(gte, 0, 0, 0, 1000);
    gte.write_control(Gte::kProjectionH, 0x100);
    gte.write_control(Gte::kOffsetX, 160u << 16);
    gte.write_control(Gte::kOffsetY, 120u << 16);

    R6_CHECK(gte.execute(with_shift(kRtps, true)));
    R6_CHECK(gte.read_data(Gte::kIr3) == 1000);
    R6_CHECK(gte.read_data(Gte::kSz3) == 1000);
    R6_CHECK(screen_x(gte, Gte::kSxy2) == 160);
    R6_CHECK(screen_y(gte, Gte::kSxy2) == 120);
    R6_CHECK(gte.read_data(Gte::kIr0) == 0);
    R6_CHECK(gte.flags() == 0);
    R6_CHECK(gte.executed() == 1);
}

// RT11 doubles as a 3.12 scale factor: 0800h halves the incoming X.
void test_rtps_scale() {
    Gte gte;
    gte.write_control(Gte::kRotation, 0x0800u);
    gte.write_control(Gte::kRotation + 2, 0x1000u);
    gte.write_control(Gte::kRotation + 4, 0x1000u);
    set_vector(gte, 0, 1000, 0, 1000);

    R6_CHECK(gte.execute(with_shift(kRtps, true)));
    R6_CHECK(gte.read_data(Gte::kIr1) == 500);
}

// A large matrix element must saturate IR1 and raise the IR1 and summary bits.
void test_rtps_ir_saturation() {
    Gte gte;
    gte.write_control(Gte::kRotation, 0x7FFFu);
    gte.write_control(Gte::kRotation + 2, 0x1000u);
    gte.write_control(Gte::kRotation + 4, 0x1000u);
    set_vector(gte, 0, 0x7FFF, 0, 1000);

    R6_CHECK(gte.execute(with_shift(kRtps, true)));
    R6_CHECK(gte.read_data(Gte::kIr1) == 0x7FFF);
    R6_CHECK((gte.flags() & Gte::kFlagIr1Saturated) != 0);
    R6_CHECK((gte.flags() & Gte::kFlagError) != 0);
}

// A vertex at or behind the camera saturates SZ3 to zero and the division
// overflows, which is the near-plane test a renderer needs to see.
void test_rtps_near_plane() {
    Gte gte;
    identity_rotation(gte);
    set_vector(gte, 0, 0, 0, -1000);
    gte.write_control(Gte::kProjectionH, 0x100);

    R6_CHECK(gte.execute(with_shift(kRtps, true)));
    R6_CHECK(gte.read_data(Gte::kSz3) == 0);
    R6_CHECK((gte.flags() & Gte::kFlagSzSaturated) != 0);
    R6_CHECK((gte.flags() & Gte::kFlagDivideOverflow) != 0);
}

// The perspective result must clamp to the -400h..+3FFh screen range.
void test_rtps_screen_clamp() {
    Gte gte;
    identity_rotation(gte);
    set_vector(gte, 0, 0x7FFF, 0, -1000);
    gte.write_control(Gte::kProjectionH, 0x100);

    R6_CHECK(gte.execute(with_shift(kRtps, true)));
    R6_CHECK(screen_x(gte, Gte::kSxy2) == 0x3FF);
    R6_CHECK((gte.flags() & Gte::kFlagSx2Saturated) != 0);
    R6_CHECK((gte.flags() & Gte::kFlagMac0Positive) != 0);
}

// RTPT runs RTPS three times, so the FIFO must end up holding V0, V1, V2 in
// order, with the Z FIFO one stage deeper.
void test_rtpt_fifo_order() {
    Gte gte;
    identity_rotation(gte);
    gte.write_control(Gte::kProjectionH, 0x100);
    set_vector(gte, 0, 1000, 0, 1000);
    set_vector(gte, 1, 1000, 0, 2000);
    set_vector(gte, 2, 1000, 0, 4000);

    R6_CHECK(gte.execute(with_shift(kRtpt, true)));
    R6_CHECK(gte.read_data(Gte::kSz3) == 4000);
    R6_CHECK(gte.read_data(Gte::kSz2) == 2000);
    R6_CHECK(gte.read_data(Gte::kSz1) == 1000);

    // Nearer vertices project further out, so the X values must be descending.
    R6_CHECK(screen_x(gte, Gte::kSxy0) > screen_x(gte, Gte::kSxy1));
    R6_CHECK(screen_x(gte, Gte::kSxy1) > screen_x(gte, Gte::kSxy2));
    R6_CHECK(screen_x(gte, Gte::kSxy2) > 0);
}

// NCLIP's sign is the winding order, and zero means the vertices are collinear.
void test_nclip_winding() {
    Gte gte;
    gte.write_data(Gte::kSxy0, pack_pair(0, 0));
    gte.write_data(Gte::kSxy1, pack_pair(100, 0));
    gte.write_data(Gte::kSxy2, pack_pair(0, 100));

    R6_CHECK(gte.execute(kNclip));
    R6_CHECK(mac0(gte) == 10000);

    gte.write_data(Gte::kSxy1, pack_pair(0, 100));
    gte.write_data(Gte::kSxy2, pack_pair(100, 0));
    R6_CHECK(gte.execute(kNclip));
    R6_CHECK(mac0(gte) == -10000);

    gte.write_data(Gte::kSxy0, pack_pair(0, 0));
    gte.write_data(Gte::kSxy1, pack_pair(50, 50));
    gte.write_data(Gte::kSxy2, pack_pair(100, 100));
    R6_CHECK(gte.execute(kNclip));
    R6_CHECK(mac0(gte) == 0);
}

void test_average_z() {
    Gte gte;
    gte.write_data(Gte::kSz1, 0x1000);
    gte.write_data(Gte::kSz2, 0x1000);
    gte.write_data(Gte::kSz3, 0x1000);
    gte.write_control(Gte::kZsf3, 0x0555);

    // (3 * 1000h) * 0555h / 1000h = 0FFFh
    R6_CHECK(gte.execute(kAvsz3));
    R6_CHECK(gte.read_data(Gte::kOtz) == 0x0FFF);

    for (std::uint32_t reg = Gte::kSz0; reg <= Gte::kSz3; ++reg) {
        gte.write_data(reg, 0x4000);
    }
    gte.write_control(Gte::kZsf4, 0x7FFF);

    // 4 * 4000h * 7FFFh / 1000h overflows 16 bits and saturates.
    R6_CHECK(gte.execute(kAvsz4));
    R6_CHECK(gte.read_data(Gte::kOtz) == 0xFFFF);
    R6_CHECK((gte.flags() & Gte::kFlagSzSaturated) != 0);
}

void test_mvmva() {
    Gte gte;
    identity_rotation(gte);
    set_vector(gte, 0, 10, 20, 30);

    R6_CHECK(gte.execute(with_shift(kMvmva, true)));
    R6_CHECK(gte.read_data(Gte::kIr1) == 10);
    R6_CHECK(gte.read_data(Gte::kIr2) == 20);
    R6_CHECK(gte.read_data(Gte::kIr3) == 30);

    // cv=1 adds the background colour instead of the translation vector.
    gte.write_control(Gte::kBackgroundR, 1000);
    gte.write_control(Gte::kBackgroundG, 2000);
    gte.write_control(Gte::kBackgroundB, 3000);
    R6_CHECK(gte.execute(with_shift(kMvmva, true) | (1u << 13)));
    R6_CHECK(gte.read_data(Gte::kIr1) == 1010);
    R6_CHECK(gte.read_data(Gte::kIr2) == 2020);
    R6_CHECK(gte.read_data(Gte::kIr3) == 3030);
}

void test_sqr() {
    Gte gte;
    gte.write_data(Gte::kIr1, 0x100);
    gte.write_data(Gte::kIr2, 0x200);
    gte.write_data(Gte::kIr3, 0x400);

    R6_CHECK(gte.execute(with_shift(kSqr, true)));
    R6_CHECK(gte.read_data(Gte::kIr1) == 16);
    R6_CHECK(gte.read_data(Gte::kIr2) == 64);
    R6_CHECK(gte.read_data(Gte::kIr3) == 256);

    // Without the shift the squares are large positives and saturate to 7FFFh.
    gte.write_data(Gte::kIr1, 0x100);
    gte.write_data(Gte::kIr2, 0x200);
    gte.write_data(Gte::kIr3, 0x400);
    R6_CHECK(gte.execute(with_shift(kSqr, false)));
    R6_CHECK(gte.read_data(Gte::kIr1) == 0x7FFF);
    R6_CHECK((gte.flags() & Gte::kFlagIr1Saturated) != 0);
    R6_CHECK((gte.flags() & Gte::kFlagIr3Saturated) != 0);
}

void test_register_behaviour() {
    Gte gte;

    // IRGB expands 5:5:5 into the IR registers; both colour registers read it
    // back.
    gte.write_data(Gte::kIrgb, 0x7FFF);
    R6_CHECK(gte.read_data(Gte::kIr1) == 0x0F80);
    R6_CHECK(gte.read_data(Gte::kIr2) == 0x0F80);
    R6_CHECK(gte.read_data(Gte::kIr3) == 0x0F80);
    R6_CHECK(gte.read_data(Gte::kIrgb) == 0x7FFF);
    R6_CHECK(gte.read_data(Gte::kOrgb) == 0x7FFF);

    // SXYP mirrors SXY2 and moves the FIFO on write.
    gte.write_data(Gte::kSxy0, pack_pair(1, 2));
    gte.write_data(Gte::kSxy1, pack_pair(3, 4));
    gte.write_data(Gte::kSxyp, pack_pair(5, 6));
    R6_CHECK(gte.read_data(Gte::kSxyp) == pack_pair(5, 6));
    R6_CHECK(screen_x(gte, Gte::kSxy0) == 3);
    R6_CHECK(screen_x(gte, Gte::kSxy2) == 5);

    // Whole-register 16-bit values read back sign-extended, Z unsigned.
    gte.write_data(Gte::kVz0, 0xFFFF);
    R6_CHECK(gte.read_data(Gte::kVz0) == 0xFFFFFFFFu);
    gte.write_data(Gte::kSz0, 0xFFFF);
    R6_CHECK(gte.read_data(Gte::kSz0) == 0xFFFFu);

    // LZCR counts leading zeroes for positive values, leading ones otherwise.
    gte.write_data(Gte::kLzcs, 0x0000FFFFu);
    R6_CHECK(gte.read_data(Gte::kLzcr) == 16);
    gte.write_data(Gte::kLzcs, 0xFFFFFFFFu);
    R6_CHECK(gte.read_data(Gte::kLzcr) == 32);
    gte.write_data(Gte::kLzcs, 0);
    R6_CHECK(gte.read_data(Gte::kLzcr) == 32);
}

void test_flags_reset_per_command() {
    Gte gte;
    gte.write_control(Gte::kFlag, 0x00FF0000u);
    identity_rotation(gte);
    set_vector(gte, 0, 0, 0, 1000);
    gte.write_control(Gte::kProjectionH, 0x100);

    R6_CHECK(gte.execute(with_shift(kRtps, true)));
    R6_CHECK(gte.flags() == 0);
}

void test_unsupported_command() {
    Gte gte;
    R6_CHECK(!gte.execute(0x4A000000u | 0x07u));
    R6_CHECK(gte.unsupported() == 1);
    R6_CHECK(gte.executed() == 0);
    R6_CHECK(gte.last_unsupported() == 0x07u);
}

// A run has to be able to say which lighting command is missing, not only how
// many were skipped, so the per-opcode totals and the summary are both checked
// here.
void test_unsupported_reporting() {
    Gte gte;
    R6_CHECK(!gte.execute(0x4A000000u | 0x14u));  // CDP, still unimplemented
    R6_CHECK(!gte.execute(0x4A000000u | 0x14u));  // CDP again
    R6_CHECK(!gte.execute(0x4A000000u | 0x1Cu));  // CC

    R6_CHECK(gte.unsupported_by_opcode()[0x14] == 2);
    R6_CHECK(gte.unsupported_by_opcode()[0x1C] == 1);
    R6_CHECK(gte.unsupported_by_opcode()[0x1B] == 0);
    R6_CHECK(gte.unsupported() == 3);

    // Busiest first, and named.
    const auto summary = gte.unsupported_summary();
    R6_CHECK(summary.find("CDP") != std::string::npos);
    R6_CHECK(summary.find("x2") != std::string::npos);
    R6_CHECK(summary.find("CDP") < summary.find("CC"));
    R6_CHECK(gte.unsupported_summary(1).find("+1 more") != std::string::npos);

    R6_CHECK(r6::Gte::opcode_name(0x1E) != nullptr);
    R6_CHECK(std::string(r6::Gte::opcode_name(0x1E)) == "NCS");
    R6_CHECK(std::string(r6::Gte::opcode_name(0x3F)) == "NCCT");
    R6_CHECK(r6::Gte::opcode_name(0x02) == nullptr);

    // An engine that has only run implemented commands reports nothing.
    Gte clean;
    R6_CHECK(clean.unsupported_summary().empty());
}

// NCS produces IR from the two matrices and leaves the colour FIFO alone.
void test_ncs_lighting() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0x1000, 0x1000);

    R6_CHECK(gte.execute(kNcs));
    R6_CHECK(gte.read_data(Gte::kIr1) == 0x1000);
    R6_CHECK(gte.read_data(Gte::kIr2) == 0x1000);
    R6_CHECK(gte.read_data(Gte::kIr3) == 0x1000);
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0);
}

// The background colour is added by the second matrix pass, in 3.12 form.
void test_ncs_background() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0, 0);
    gte.write_control(Gte::kBackgroundR, 0x1000);

    R6_CHECK(gte.execute(kNcs));
    R6_CHECK(gte.read_data(Gte::kIr1) == 0x2000);
    R6_CHECK(gte.read_data(Gte::kIr2) == 0);
}

// Neutral primary colour at neutral light is the identity for NCCS.
void test_nccs_primary_colour() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0x1000, 0x1000);
    gte.write_data(Gte::kRgbc, 0x00808080u);

    R6_CHECK(gte.execute(kNccs));
    R6_CHECK(gte.read_data(Gte::kIr1) == 0x800);
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00808080u);
}

// NCDS interpolates towards the far colour by IR0: zero leaves the lit colour
// alone, one replaces it with the far colour.
void test_ncds_depth_cue() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0x1000, 0x1000);
    gte.write_data(Gte::kRgbc, 0x00808080u);

    gte.write_data(Gte::kIr0, 0);
    R6_CHECK(gte.execute(kNcds));
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00808080u);

    gte.write_control(Gte::kFarColorR, 0x100);
    gte.write_control(Gte::kFarColorG, 0x100);
    gte.write_control(Gte::kFarColorB, 0x100);
    gte.write_data(Gte::kRgbc, 0x00808080u);
    gte.write_data(Gte::kIr0, 0x1000);
    R6_CHECK(gte.execute(kNcds));
    // The cue compares against the pre-shift MAC (R*IR SHL 4 = 800000h), so
    // with IR0 = 1.0 the whole entry becomes the far colour: MAC = 100000h,
    // MAC SAR 12 = 100h, and the FIFO stores MAC/16 = 10h per channel.
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00101010u);
}

void test_colour_fifo_order() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0x1000, 0x1000);

    gte.write_data(Gte::kRgbc, 0x00000010u);  // red = 10h
    R6_CHECK(gte.execute(kNccs));
    gte.write_data(Gte::kRgbc, 0x00002000u);  // green = 20h
    R6_CHECK(gte.execute(kNccs));

    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00002000u);
    R6_CHECK(gte.read_data(Gte::kRgb1) == 0x00000010u);
    R6_CHECK(gte.read_data(Gte::kRgb0) == 0);
}

// A blowing-out colour saturates per channel and raises the colour flags.
void test_nccs_colour_saturation() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x7FFF, 0x7FFF, 0x7FFF);
    gte.write_data(Gte::kRgbc, 0x00FFFFFFu);

    R6_CHECK(gte.execute(kNccs));
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00FFFFFFu);
    R6_CHECK((gte.flags() & Gte::kFlagColorRSaturated) != 0);
    R6_CHECK((gte.flags() & Gte::kFlagColorGSaturated) != 0);
    R6_CHECK((gte.flags() & Gte::kFlagColorBSaturated) != 0);
}

// The top byte of RGBC is the GPU command code and must survive into the FIFO.
void test_nccs_code_byte() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0x1000, 0x1000);
    gte.write_data(Gte::kRgbc, 0x3C008080u);

    R6_CHECK(gte.execute(kNccs));
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x3C008080u);
}

// NCT is the NCS chain for V0, V1 and V2: IR ends on the last vertex and the
// colour FIFO is still untouched.
void test_nct_ir_only() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0, 0);
    set_vector(gte, 1, 0, 0x1000, 0);
    set_vector(gte, 2, 0, 0, 0x0800);
    gte.write_data(Gte::kRgbc, 0x00808080u);

    R6_CHECK(gte.execute(kNct));
    R6_CHECK(gte.read_data(Gte::kIr3) == 0x0800);
    R6_CHECK(gte.read_data(Gte::kIr1) == 0);
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0);
}

// NCCT pushes one colour-FIFO entry per vertex, so the FIFO ends up holding V0,
// V1 and V2 in order. Each vertex lights a different channel here.
void test_ncct_fifo_order() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0, 0);
    set_vector(gte, 1, 0, 0x1000, 0);
    set_vector(gte, 2, 0, 0, 0x1000);
    gte.write_data(Gte::kRgbc, 0x00808080u);

    R6_CHECK(gte.execute(kNcct));
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00800000u);  // V2 lit blue
    R6_CHECK(gte.read_data(Gte::kRgb1) == 0x00008000u);  // V1 lit green
    R6_CHECK(gte.read_data(Gte::kRgb0) == 0x00000080u);  // V0 lit red
}

// NCDT applies the depth cue to each vertex in turn.
void test_ncdt_depth_cue() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0, 0);
    set_vector(gte, 1, 0, 0x1000, 0);
    set_vector(gte, 2, 0, 0, 0x1000);
    gte.write_data(Gte::kRgbc, 0x00808080u);
    for (const auto reg : {Gte::kFarColorR, Gte::kFarColorG, Gte::kFarColorB}) {
        gte.write_control(reg, 0x100);
    }
    gte.write_data(Gte::kIr0, 0x1000);

    R6_CHECK(gte.execute(kNcdt));
    // IR0 = 1.0 pulls every vertex to the far colour: 100h/16 = 10h.
    R6_CHECK(gte.read_data(Gte::kRgb2) == 0x00101010u);
    R6_CHECK(gte.read_data(Gte::kRgb1) == 0x00101010u);
    R6_CHECK(gte.read_data(Gte::kRgb0) == 0x00101010u);
}

void test_lighting_triples_implemented() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0, 0);
    gte.write_data(Gte::kRgbc, 0x00808080u);

    R6_CHECK(gte.execute(kNct));
    R6_CHECK(gte.execute(kNcct));
    R6_CHECK(gte.execute(kNcdt));
    R6_CHECK(gte.executed() == 3);
    R6_CHECK(gte.unsupported() == 0);
    R6_CHECK(gte.unsupported_by_opcode()[0x20] == 0);
    R6_CHECK(gte.unsupported_by_opcode()[0x3F] == 0);
    R6_CHECK(gte.unsupported_by_opcode()[0x16] == 0);
}

void test_lighting_commands_implemented() {
    Gte gte;
    identity_lighting(gte);
    set_vector(gte, 0, 0x1000, 0, 0);
    gte.write_data(Gte::kRgbc, 0x00808080u);

    R6_CHECK(gte.execute(kNcs));
    R6_CHECK(gte.execute(kNccs));
    R6_CHECK(gte.execute(kNcds));
    R6_CHECK(gte.executed() == 3);
    R6_CHECK(gte.unsupported() == 0);
    R6_CHECK(gte.unsupported_by_opcode()[0x1E] == 0);
    R6_CHECK(gte.unsupported_by_opcode()[0x1B] == 0);
    R6_CHECK(gte.unsupported_by_opcode()[0x13] == 0);
}

// The engine is only useful if COP2 dispatches to it, so this drives the
// interpreter itself: rtps, then mfc2 of SXY2, then break.
void test_interpreter_cop2_path() {
    r6::Memory ram(2u * 1024u * 1024u, true);
    r6::NullPlatformServices services;
    r6::MipsInterpreter interpreter(ram, services);

    constexpr std::uint32_t kProgramOffset = 0x00010000u;
    ram.write32(kProgramOffset + 0, 0x4A000000u | kRtps);
    // mfc2 $t0, $14: opcode 12h, rs=0 (mfc2), rt=8, rd=14 at bits 11-15.
    ram.write32(kProgramOffset + 4, 0x48000000u | (8u << 16) | (14u << 11));
    ram.write32(kProgramOffset + 8, 0x0000000Du);  // break

    auto& gte = interpreter.gte();
    identity_rotation(gte);
    set_vector(gte, 0, 0, 0, 1000);
    gte.write_control(Gte::kProjectionH, 0x100);
    gte.write_control(Gte::kOffsetX, 100u << 16);

    interpreter.cpu().pc = 0x80000000u | kProgramOffset;
    const auto result = interpreter.run(8);

    R6_CHECK(result.reason == r6::StopReason::guest_break);
    R6_CHECK(interpreter.gte_commands() == 1);
    R6_CHECK(interpreter.gte_unsupported_commands() == 0);
    R6_CHECK(interpreter.cpu().gpr[8] == pack_pair(100, 0));
}

}  // namespace

void run_gte_tests() {
    test_rtps_identity();
    test_rtps_scale();
    test_rtps_ir_saturation();
    test_rtps_near_plane();
    test_rtps_screen_clamp();
    test_rtpt_fifo_order();
    test_nclip_winding();
    test_average_z();
    test_mvmva();
    test_sqr();
    test_register_behaviour();
    test_flags_reset_per_command();
    test_unsupported_command();
    test_unsupported_reporting();
    test_ncs_lighting();
    test_ncs_background();
    test_nccs_primary_colour();
    test_ncds_depth_cue();
    test_colour_fifo_order();
    test_nccs_colour_saturation();
    test_nccs_code_byte();
    test_lighting_commands_implemented();
    test_nct_ir_only();
    test_ncct_fifo_order();
    test_ncdt_depth_cue();
    test_lighting_triples_implemented();
    test_interpreter_cop2_path();
}
