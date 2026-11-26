#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace r6 {

struct TexturedVertex {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint8_t u{0};
    std::uint8_t v{0};
};

// PlayStation GPU command decoder and VRAM rasterizer.
//
// VRAM is modelled as 1024x512 16-bit BGR555 pixels, matching the retail GPU
// layout. The decoder tracks how many parameter words each GP0 command needs,
// so an unsupported command cannot desynchronise the following command stream.
//
// Textured primitives are decoded and counted but not rendered yet; gouraud
// shading is interpolated per pixel so shaded geometry is drawn instead of being
// reported as unsupported.
class SoftwareGpu {
public:
    static constexpr std::uint32_t kVramWidth = 1024;
    static constexpr std::uint32_t kVramHeight = 512;
    static constexpr std::uint32_t kDisplayWidth = 320;
    static constexpr std::uint32_t kDisplayHeight = 240;

    SoftwareGpu();

    // GP0 command/data port (writes to 0x1F801810).
    void submit_gp0(std::uint32_t word);
    // GP1 control port (writes to 0x1F801814).
    void submit_gp1(std::uint32_t word);
    // GPUSTAT (reads from 0x1F801814).
    std::uint32_t status() const;
    // GPUREAD (reads from 0x1F801810) after a VRAM-to-CPU command.
    std::uint32_t read_gp0_data();

    std::uint16_t read_vram(std::uint32_t x, std::uint32_t y) const;

    // 32-bit XRGB view of the selected display area, for host presentation.
    std::vector<std::uint32_t> display_frame() const;
    // Count of non-zero VRAM pixels, to tell "nothing drawn" apart from
    // "drawn outside the display area" during bring-up.
    std::uint32_t filled_vram_pixels() const;
    // Bounding box of the non-zero VRAM pixels. Tells "drawn outside the
    // display window" apart from "nothing drawn at all". False when empty.
    bool filled_vram_bounds(std::uint32_t& left, std::uint32_t& top,
                            std::uint32_t& right,
                            std::uint32_t& bottom) const;

    std::uint32_t rendered_primitives() const { return rendered_primitives_; }
    std::uint32_t unsupported_commands() const { return unsupported_commands_; }
    std::uint32_t unsupported_commands(std::uint8_t opcode) const;
    std::uint8_t last_unsupported_opcode() const {
        return last_unsupported_opcode_;
    }
    std::uint8_t last_gp1_command() const { return last_gp1_command_; }
    std::uint32_t gp1_writes() const { return gp1_writes_; }
    std::uint32_t display_enable_writes() const {
        return display_enable_writes_;
    }
    std::uint32_t last_display_enable_word() const {
        return last_display_enable_word_;
    }
    bool display_enabled() const { return display_enabled_; }
    std::uint32_t fill_rectangles() const { return fill_rectangles_; }
    std::int32_t draw_area_left() const { return draw_area_left_; }
    std::int32_t draw_area_top() const { return draw_area_top_; }
    std::int32_t draw_area_right() const { return draw_area_right_; }
    std::int32_t draw_area_bottom() const { return draw_area_bottom_; }
    std::uint32_t display_area_x() const {
        return static_cast<std::uint32_t>(display_area_x_);
    }
    std::uint32_t display_area_y() const {
        return static_cast<std::uint32_t>(display_area_y_);
    }

    // Number of 32-bit words a GP0 packet consumes. Zero means the packet has a
    // data-dependent length (poly-lines and CPU-to-VRAM uploads).
    static std::size_t gp0_word_count(std::uint8_t opcode);

private:
    void reset_registers();
    bool packet_complete() const;
    void execute_gp0();
    void execute_environment(std::uint8_t opcode);
    void fill_rectangle(std::uint32_t rgb,
                        std::uint32_t position,
                        std::uint32_t size);
    void draw_rectangle(std::uint32_t rgb,
                        std::uint32_t position,
                        std::uint32_t width,
                        std::uint32_t height);
    void draw_triangle(std::int32_t x0,
                       std::int32_t y0,
                       std::int32_t x1,
                       std::int32_t y1,
                       std::int32_t x2,
                       std::int32_t y2,
                       std::uint16_t color);
    void draw_gouraud_triangle(std::int32_t x0,
                                std::int32_t y0,
                                std::uint16_t color0,
                                std::int32_t x1,
                                std::int32_t y1,
                                std::uint16_t color1,
                                std::int32_t x2,
                                std::int32_t y2,
                                std::uint16_t color2);
    void draw_textured_triangle(const TexturedVertex& first,
                                const TexturedVertex& second,
                                const TexturedVertex& third,
                                std::uint32_t modulation,
                                std::uint32_t clut,
                                std::uint32_t texpage,
                                bool semi_transparent);
    void draw_textured_rectangle(std::uint32_t modulation,
                                 std::uint32_t position,
                                 std::uint32_t texcoord,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t clut,
                                 std::uint32_t texpage,
                                 bool semi_transparent);
    std::uint16_t sample_texture(std::uint32_t u,
                                 std::uint32_t v,
                                 std::uint32_t clut,
                                 std::uint32_t texpage) const;
    std::uint32_t texture_window_coordinate(std::uint32_t coordinate,
                                            bool horizontal) const;
    void plot_textured(std::int32_t x,
                       std::int32_t y,
                       std::uint16_t texel,
                       std::uint32_t modulation,
                       std::uint32_t texpage,
                       bool semi_transparent);
    void copy_vram(std::uint32_t source,
                   std::uint32_t destination,
                   std::uint32_t size);
    void upload_image();
    void begin_download(std::uint32_t position, std::uint32_t size);
    void store_pixel(std::int32_t x, std::int32_t y, std::uint16_t color);
    void write_pixel(std::int32_t x, std::int32_t y, std::uint16_t color);
    void record_unsupported(std::uint8_t opcode);

    std::vector<std::uint16_t> vram_;
    std::vector<std::uint32_t> packet_;
    std::size_t expected_words_{0};

    std::int32_t draw_area_left_{0};
    std::int32_t draw_area_top_{0};
    std::int32_t draw_area_right_{static_cast<std::int32_t>(kVramWidth) - 1};
    std::int32_t draw_area_bottom_{static_cast<std::int32_t>(kVramHeight) - 1};
    std::int32_t draw_offset_x_{0};
    std::int32_t draw_offset_y_{0};
    std::uint32_t texpage_{0};
    std::uint32_t texture_window_{0};
    std::uint32_t mask_bit_setting_{0};
    std::uint32_t display_mode_{0};
    std::uint32_t dma_direction_{0};
    std::int32_t display_area_x_{0};
    std::int32_t display_area_y_{0};
    bool display_enabled_{false};

    std::vector<std::uint16_t> download_words_;
    std::size_t download_index_{0};

    std::uint32_t rendered_primitives_{0};
    std::uint32_t fill_rectangles_{0};
    std::uint32_t unsupported_commands_{0};
    std::map<std::uint8_t, std::uint32_t> unsupported_by_opcode_;
    std::uint8_t last_unsupported_opcode_{0};
    std::uint8_t last_gp1_command_{0};
    std::uint32_t gp1_writes_{0};
    std::uint32_t display_enable_writes_{0};
    std::uint32_t last_display_enable_word_{0};
};

}  // namespace r6
