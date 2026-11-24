#include "test_support.h"

#include <cstdint>
#include <initializer_list>

#include "r6/software_gpu.h"

namespace {

using r6::SoftwareGpu;

// CPU-to-VRAM upload of one scanline of halfwords.
void upload_halfwords(SoftwareGpu& gpu, std::uint32_t x, std::uint32_t y,
                      std::initializer_list<std::uint16_t> halfwords) {
    const auto count = static_cast<std::uint32_t>(halfwords.size());
    gpu.submit_gp0(0xA0000000u);
    gpu.submit_gp0((y << 16) | x);
    gpu.submit_gp0((1u << 16) | count);

    std::uint32_t word = 0;
    std::uint32_t filled = 0;
    for (const auto value : halfwords) {
        word |= static_cast<std::uint32_t>(value) << (filled * 16u);
        ++filled;
        if (filled == 2) {
            gpu.submit_gp0(word);
            word = 0;
            filled = 0;
        }
    }
    if (filled != 0) gpu.submit_gp0(word);
}

void packet_lengths() {
    R6_CHECK(SoftwareGpu::gp0_word_count(0x00) == 1);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x02) == 3);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x20) == 4);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x24) == 7);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x2C) == 9);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x3C) == 12);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x40) == 3);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x60) == 3);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x70) == 2);
    R6_CHECK(SoftwareGpu::gp0_word_count(0x80) == 4);
    R6_CHECK(SoftwareGpu::gp0_word_count(0xA0) == 0);
    R6_CHECK(SoftwareGpu::gp0_word_count(0xE4) == 1);
}

void fill_rounds_to_sixteen_pixels() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x020000FFu);  // fill, red
    gpu.submit_gp0(0x00000011u);  // x=17, y=0
    gpu.submit_gp0(0x00010001u);  // 1x1
    R6_CHECK(gpu.read_vram(16, 0) == 0x001Fu);
    R6_CHECK(gpu.read_vram(31, 0) == 0x001Fu);
    R6_CHECK(gpu.read_vram(32, 0) == 0x0000u);
    R6_CHECK(gpu.rendered_primitives() == 1);
}

void monochrome_rectangle() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x600000FFu);  // rectangle, red
    gpu.submit_gp0(0x0014000Au);  // x=10, y=20
    gpu.submit_gp0(0x00020002u);  // 2x2
    R6_CHECK(gpu.read_vram(10, 20) == 0x001Fu);
    R6_CHECK(gpu.read_vram(11, 21) == 0x001Fu);
    R6_CHECK(gpu.read_vram(12, 20) == 0x0000u);
}

void drawing_area_clips_primitives() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE3000000u);  // drawing area top-left (0,0)
    gpu.submit_gp0(0xE4001405u);  // bottom-right x=5, y=5
    gpu.submit_gp0(0x600000FFu);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x000A000Au);  // 10x10
    R6_CHECK(gpu.read_vram(5, 5) == 0x001Fu);
    R6_CHECK(gpu.read_vram(6, 6) == 0x0000u);
}

void drawing_offset_shifts_vertices() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE53FE7FCu);  // offset (-4, -4) in 11-bit fields
    gpu.submit_gp0(0x600000FFu);
    gpu.submit_gp0(0x00140014u);  // x=20, y=20
    gpu.submit_gp0(0x00010001u);
    R6_CHECK(gpu.read_vram(16, 16) == 0x001Fu);
    R6_CHECK(gpu.read_vram(20, 20) == 0x0000u);
}

void monochrome_triangle() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x200000FFu);
    gpu.submit_gp0(0x00000000u);  // (0,0)
    gpu.submit_gp0(0x000A0000u);  // (0,10)
    gpu.submit_gp0(0x0000000Au);  // (10,0)
    R6_CHECK(gpu.read_vram(1, 1) == 0x001Fu);
    R6_CHECK(gpu.read_vram(0, 9) == 0x001Fu);
    R6_CHECK(gpu.read_vram(9, 9) == 0x0000u);
}

void unsupported_packet_keeps_stream_in_sync() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x30FF0000u);  // gouraud triangle, not rendered yet
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00000000u);
    R6_CHECK(gpu.unsupported_commands() == 1);
    R6_CHECK(gpu.unsupported_commands(0x30) == 1);
    R6_CHECK(gpu.last_unsupported_opcode() == 0x30);

    gpu.submit_gp0(0x6000FF00u);  // green rectangle follows
    gpu.submit_gp0(0x00000020u);  // x=32, y=0
    gpu.submit_gp0(0x00010001u);
    R6_CHECK(gpu.read_vram(32, 0) == 0x03E0u);
    R6_CHECK(gpu.rendered_primitives() == 1);
}

void vram_copy() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x020000FFu);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00010010u);  // 16x1 red
    gpu.submit_gp0(0x80000000u);
    gpu.submit_gp0(0x00000000u);  // source (0,0)
    gpu.submit_gp0(0x00000100u);  // destination (256,0)
    gpu.submit_gp0(0x00010010u);  // 16x1
    R6_CHECK(gpu.read_vram(256, 0) == 0x001Fu);
    R6_CHECK(gpu.read_vram(0, 0) == 0x001Fu);
}

void cpu_to_vram_upload() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xA0000000u);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00010002u);  // 2x1
    gpu.submit_gp0(0xBBBBAAAAu);
    R6_CHECK(gpu.read_vram(0, 0) == 0xAAAAu);
    R6_CHECK(gpu.read_vram(1, 0) == 0xBBBBu);
}

void vram_to_cpu_download() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x020000FFu);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00010010u);
    gpu.submit_gp0(0xC0000000u);
    gpu.submit_gp0(0x00000000u);
    gpu.submit_gp0(0x00010002u);
    R6_CHECK(gpu.read_gp0_data() == 0x001F001Fu);
    R6_CHECK(gpu.read_gp0_data() == 0);
}

void status_and_display_control() {
    SoftwareGpu gpu;
    R6_CHECK((gpu.status() & 0x1C000000u) == 0x1C000000u);
    R6_CHECK(!gpu.display_enabled());
    R6_CHECK((gpu.status() & (1u << 23)) != 0);

    gpu.submit_gp1(0x03000001u);
    R6_CHECK(gpu.display_enabled());
    R6_CHECK((gpu.status() & (1u << 23)) == 0);
    R6_CHECK(gpu.last_gp1_command() == 0x03);
}

void display_frame_uses_display_area() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0x020000FFu);
    gpu.submit_gp0(0x00100000u);  // x=0, y=16
    gpu.submit_gp0(0x00010010u);
    gpu.submit_gp1(0x05000400u);  // display area start y=1 -> 0x400

    R6_CHECK(gpu.display_area_y() == 1);
    const auto frame = gpu.display_frame();
    R6_CHECK(frame.size() ==
             static_cast<std::size_t>(SoftwareGpu::kDisplayWidth) *
                 SoftwareGpu::kDisplayHeight);
    R6_CHECK(frame[static_cast<std::size_t>(15) * SoftwareGpu::kDisplayWidth] ==
             0xFFFF0000u);
}

void gp1_reset_restores_drawing_area() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE3000000u);
    gpu.submit_gp0(0xE4000000u);  // 1x1 drawing area
    gpu.submit_gp1(0x00000000u);
    gpu.submit_gp0(0x600000FFu);
    gpu.submit_gp0(0x00640064u);  // x=100, y=100
    gpu.submit_gp0(0x00010001u);
    R6_CHECK(gpu.read_vram(100, 100) == 0x001Fu);
}

void textured_rectangle_4bit_clut() {
    SoftwareGpu gpu;
    upload_halfwords(gpu, 0, 0, {0x3210u});  // texels 0,1,2,3
    upload_halfwords(gpu, 16, 0,
                     {0x0000u, 0x001Fu, 0x03E0u, 0x7C00u});

    gpu.submit_gp0(0x64808080u);      // textured rectangle, neutral colour
    gpu.submit_gp0(32u << 16);        // at (0,32)
    gpu.submit_gp0(1u << 16);         // uv (0,0), CLUT x=16 y=0
    gpu.submit_gp0((1u << 16) | 4u);  // 4x1

    R6_CHECK(gpu.read_vram(0, 32) == 0x0000u);  // palette entry 0 is clear
    R6_CHECK(gpu.read_vram(1, 32) == 0x001Fu);
    R6_CHECK(gpu.read_vram(2, 32) == 0x03E0u);
    R6_CHECK(gpu.read_vram(3, 32) == 0x7C00u);
    R6_CHECK(gpu.rendered_primitives() == 1);
}

void textured_rectangle_8bit_clut() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE1000080u);  // 8-bit texture page
    upload_halfwords(gpu, 0, 0, {0x0201u});
    upload_halfwords(gpu, 16, 0,
                     {0x0000u, 0x001Fu, 0x03E0u, 0x7C00u});

    gpu.submit_gp0(0x64808080u);
    gpu.submit_gp0((34u << 16));
    gpu.submit_gp0(1u << 16);
    gpu.submit_gp0((1u << 16) | 2u);

    R6_CHECK(gpu.read_vram(0, 34) == 0x001Fu);
    R6_CHECK(gpu.read_vram(1, 34) == 0x03E0u);
}

void textured_rectangle_15bit_direct() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE1000100u);  // 15-bit direct texture page
    upload_halfwords(gpu, 0, 0, {0x7C00u});

    gpu.submit_gp0(0x64808080u);
    gpu.submit_gp0((36u << 16));
    gpu.submit_gp0(0u);
    gpu.submit_gp0((1u << 16) | 1u);

    R6_CHECK(gpu.read_vram(0, 36) == 0x7C00u);
}

void transparent_texel_leaves_destination() {
    SoftwareGpu gpu;
    upload_halfwords(gpu, 0, 0, {0x3210u});
    upload_halfwords(gpu, 16, 0,
                     {0x0000u, 0x001Fu, 0x03E0u, 0x7C00u});

    gpu.submit_gp0(0x0200FF00u);        // fill green
    gpu.submit_gp0(38u << 16);          // at (0,38)
    gpu.submit_gp0((1u << 16) | 16u);

    gpu.submit_gp0(0x64808080u);
    gpu.submit_gp0((38u << 16));
    gpu.submit_gp0(1u << 16);  // texel index 0 is transparent
    gpu.submit_gp0((1u << 16) | 1u);

    R6_CHECK(gpu.read_vram(0, 38) == 0x03E0u);
}

void texture_modulation_and_semi_transparency() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE1000100u);             // 15-bit page
    upload_halfwords(gpu, 0, 0, {0xFFFFu});  // white, semi-transparent flag

    gpu.submit_gp0(0x02FF0000u);  // fill blue
    gpu.submit_gp0(40u << 16);    // at (0,40)
    gpu.submit_gp0((1u << 16) | 16u);

    gpu.submit_gp0(0x64404040u);  // opaque, colour halved
    gpu.submit_gp0((40u << 16) | 1u);
    gpu.submit_gp0(0u);
    gpu.submit_gp0((1u << 16) | 1u);
    R6_CHECK(gpu.read_vram(1, 40) == 0x3DEFu);

    gpu.submit_gp0(0x66404040u);  // semi-transparent, B/2+F/2
    gpu.submit_gp0((40u << 16));
    gpu.submit_gp0(0u);
    gpu.submit_gp0((1u << 16) | 1u);
    R6_CHECK(gpu.read_vram(0, 40) == 0x5CE7u);
}

void textured_triangle_interpolates_uv() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE1000100u);  // 15-bit page
    upload_halfwords(gpu, 0, 0,
                     {0x001Fu, 0x03E0u, 0x7C00u, 0x7FFFu});

    gpu.submit_gp0(0x24808080u);  // textured triangle, opaque
    gpu.submit_gp0((70u << 16));        // (0,70)
    gpu.submit_gp0(0u);                 // uv (0,0)
    gpu.submit_gp0((70u << 16) | 4u);   // (4,70)
    gpu.submit_gp0((0x0100u << 16) | 4u);  // uv (4,0), texpage
    gpu.submit_gp0((74u << 16));        // (0,74)
    gpu.submit_gp0(0u);                 // uv (0,0)

    R6_CHECK(gpu.read_vram(1, 71) == 0x03E0u);
    R6_CHECK(gpu.read_vram(3, 73) == 0x0000u);
}

void texture_window_wraps_coordinates() {
    SoftwareGpu gpu;
    gpu.submit_gp0(0xE1000100u);  // 15-bit page
    upload_halfwords(gpu, 0, 0,
                     {0x001Fu, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x7C00u});
    gpu.submit_gp0(0xE2000001u);  // mask X = 1 -> repeating 8-texel window

    gpu.submit_gp0(0x64808080u);
    gpu.submit_gp0((80u << 16));
    gpu.submit_gp0(8u);  // u = 8 wraps back to 0
    gpu.submit_gp0((1u << 16) | 1u);

    R6_CHECK(gpu.read_vram(0, 80) == 0x001Fu);
}

}  // namespace

void run_software_gpu_tests() {
    packet_lengths();
    fill_rounds_to_sixteen_pixels();
    monochrome_rectangle();
    drawing_area_clips_primitives();
    drawing_offset_shifts_vertices();
    monochrome_triangle();
    unsupported_packet_keeps_stream_in_sync();
    vram_copy();
    cpu_to_vram_upload();
    vram_to_cpu_download();
    status_and_display_control();
    display_frame_uses_display_area();
    gp1_reset_restores_drawing_area();
    textured_rectangle_4bit_clut();
    textured_rectangle_8bit_clut();
    textured_rectangle_15bit_direct();
    transparent_texel_leaves_destination();
    texture_modulation_and_semi_transparency();
    textured_triangle_interpolates_uv();
    texture_window_wraps_coordinates();
}
