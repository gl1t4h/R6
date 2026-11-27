#include "r6/software_gpu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace r6 {

namespace {

constexpr std::uint8_t opcode_of(std::uint32_t word) {
    return static_cast<std::uint8_t>(word >> 24);
}

// PlayStation colours are 24-bit 0xBBGGRR; VRAM pixels are 15-bit BGR555.
constexpr std::uint16_t to_bgr555(std::uint32_t rgb) {
    const auto red = static_cast<std::uint16_t>((rgb >> 3) & 0x1Fu);
    const auto green = static_cast<std::uint16_t>((rgb >> 11) & 0x1Fu);
    const auto blue = static_cast<std::uint16_t>((rgb >> 19) & 0x1Fu);
    return static_cast<std::uint16_t>(red | (green << 5) | (blue << 10));
}

constexpr std::uint32_t expand5(std::uint16_t value) {
    return (static_cast<std::uint32_t>(value) << 3) |
           (static_cast<std::uint32_t>(value) >> 2);
}

constexpr std::uint32_t to_xrgb(std::uint16_t pixel) {
    const auto red = expand5(static_cast<std::uint16_t>(pixel & 0x1Fu));
    const auto green = expand5(static_cast<std::uint16_t>((pixel >> 5) & 0x1Fu));
    const auto blue = expand5(static_cast<std::uint16_t>((pixel >> 10) & 0x1Fu));
    return 0xFF000000u | (red << 16) | (green << 8) | blue;
}

constexpr std::int32_t coordinate_x(std::uint32_t word) {
    return static_cast<std::int32_t>(word & 0xFFFFu);
}

constexpr std::int32_t coordinate_y(std::uint32_t word) {
    return static_cast<std::int32_t>((word >> 16) & 0xFFFFu);
}

constexpr std::uint32_t rect_width(std::uint32_t word) {
    return word & 0x3FFu;
}

constexpr std::uint32_t rect_height(std::uint32_t word) {
    return (word >> 16) & 0x1FFu;
}

// Texture modulation: (texel * vertex colour) / 128 per channel.
constexpr std::uint16_t modulate_texel(std::uint16_t texel,
                                      std::uint32_t color) {
    const auto red = std::min<std::uint32_t>(
        31u, ((texel & 0x1Fu) * (color & 0xFFu)) / 128u);
    const auto green = std::min<std::uint32_t>(
        31u, (((texel >> 5) & 0x1Fu) * ((color >> 8) & 0xFFu)) / 128u);
    const auto blue = std::min<std::uint32_t>(
        31u, (((texel >> 10) & 0x1Fu) * ((color >> 16) & 0xFFu)) / 128u);
    return static_cast<std::uint16_t>(red | (green << 5) | (blue << 10));
}

// Semi-transparency modes: 0 = B/2+F/2, 1 = B+F, 2 = B-F, 3 = B+F/4.
constexpr std::uint16_t blend_texel(std::uint16_t back,
                                   std::uint16_t front,
                                   std::uint32_t mode) {
    const auto channel = [mode](std::uint32_t back_channel,
                                std::uint32_t front_channel) {
        switch (mode & 0x3u) {
            case 0:
                return (back_channel + front_channel) >> 1;
            case 1:
                return std::min<std::uint32_t>(31u,
                                               back_channel + front_channel);
            case 2:
                return back_channel > front_channel
                           ? back_channel - front_channel
                           : 0u;
            default:
                return std::min<std::uint32_t>(
                    31u, back_channel + front_channel / 4u);
        }
    };
    const auto red = channel(back & 0x1Fu, front & 0x1Fu);
    const auto green = channel((back >> 5) & 0x1Fu, (front >> 5) & 0x1Fu);
    const auto blue = channel((back >> 10) & 0x1Fu, (front >> 10) & 0x1Fu);
    return static_cast<std::uint16_t>(red | (green << 5) | (blue << 10));
}

constexpr std::int32_t signed_offset(std::uint32_t value) {
    const auto raw = static_cast<std::int32_t>(value & 0x7FFu);
    return (raw & 0x400) != 0 ? raw - 0x800 : raw;
}

constexpr bool is_upload(std::uint8_t opcode) {
    return opcode >= 0xA0 && opcode <= 0xBF;
}

constexpr bool is_polyline(std::uint8_t opcode) {
    return (opcode >= 0x48 && opcode <= 0x4F) ||
           (opcode >= 0x58 && opcode <= 0x5F);
}

// A poly-line packet is terminated by a vertex with the high bit set. The cap
// keeps a corrupt stream from buffering without bound.
constexpr std::size_t kMaxPolylineWords = 64;

}  // namespace

SoftwareGpu::SoftwareGpu()
    : vram_(static_cast<std::size_t>(kVramWidth) * kVramHeight, 0) {}

std::size_t SoftwareGpu::gp0_word_count(std::uint8_t opcode) {
    switch (opcode) {
        case 0x00:  // nop
        case 0x01:  // clear texture cache
        case 0x03:  // timing control
        case 0x1F:  // interrupt request
            return 1;
        case 0x02:  // fill rectangle
            return 3;
        default:
            break;
    }

    if (opcode >= 0x20 && opcode <= 0x23) return 4;   // monochrome triangle
    if (opcode <= 0x27) return 7;                     // textured triangle
    if (opcode <= 0x2B) return 5;                     // monochrome quad
    if (opcode <= 0x2F) return 9;                     // textured quad
    if (opcode <= 0x33) return 6;                     // gouraud triangle
    if (opcode <= 0x37) return 9;                     // gouraud textured triangle
    if (opcode <= 0x3B) return 8;                     // gouraud quad
    if (opcode <= 0x3F) return 12;                    // gouraud textured quad
    if (opcode >= 0x40 && opcode <= 0x47) return 3;   // monochrome line
    if (opcode >= 0x48 && opcode <= 0x4F) return 0;   // monochrome poly-line
    if (opcode >= 0x50 && opcode <= 0x57) return 4;   // gouraud line
    if (opcode >= 0x58 && opcode <= 0x5F) return 0;   // gouraud poly-line
    if (opcode >= 0x60 && opcode <= 0x63) return 3;   // variable rectangle
    if (opcode <= 0x67) return 4;                     // textured variable rect
    if (opcode <= 0x6B) return 2;                     // 1x1 dot
    if (opcode <= 0x6F) return 3;                     // textured 1x1
    if (opcode <= 0x73) return 2;                     // 8x8 rectangle
    if (opcode <= 0x77) return 3;                     // textured 8x8
    if (opcode <= 0x7B) return 2;                     // 16x16 rectangle
    if (opcode <= 0x7F) return 3;                     // textured 16x16
    if (opcode >= 0x80 && opcode <= 0x9F) return 4;   // VRAM-to-VRAM copy
    if (opcode >= 0xA0 && opcode <= 0xBF) return 0;   // CPU-to-VRAM upload
    if (opcode >= 0xC0 && opcode <= 0xDF) return 3;   // VRAM-to-CPU download
    if (opcode >= 0xE0 && opcode <= 0xE6) return 1;   // environment
    return 1;
}

void SoftwareGpu::reset_registers() {
    draw_area_left_ = 0;
    draw_area_top_ = 0;
    draw_area_right_ = static_cast<std::int32_t>(kVramWidth) - 1;
    draw_area_bottom_ = static_cast<std::int32_t>(kVramHeight) - 1;
    draw_offset_x_ = 0;
    draw_offset_y_ = 0;
    texpage_ = 0;
    texture_window_ = 0;
    mask_bit_setting_ = 0;
    display_mode_ = 0;
    dma_direction_ = 0;
    display_area_x_ = 0;
    display_area_y_ = 0;
    display_enabled_ = false;
}

void SoftwareGpu::submit_gp0(std::uint32_t word) {
    if (packet_.empty()) {
        expected_words_ = gp0_word_count(opcode_of(word));
        packet_.push_back(word);
        if (expected_words_ == 1) {
            execute_gp0();
            packet_.clear();
            expected_words_ = 0;
        }
        return;
    }

    packet_.push_back(word);
    if (!packet_complete()) return;
    execute_gp0();
    packet_.clear();
    expected_words_ = 0;
}

bool SoftwareGpu::packet_complete() const {
    if (packet_.empty()) return false;
    const auto opcode = opcode_of(packet_.front());
    if (expected_words_ != 0) return packet_.size() >= expected_words_;
    if (is_upload(opcode)) {
        if (packet_.size() < 3) return false;
        const std::size_t pixels =
            static_cast<std::size_t>(rect_width(packet_[2])) *
            rect_height(packet_[2]);
        return packet_.size() >= 3 + (pixels + 1) / 2;
    }
    if (is_polyline(opcode)) {
        if (packet_.size() >= kMaxPolylineWords) return true;
        return packet_.size() >= 4 && (packet_.back() & 0x80000000u) != 0;
    }
    return true;
}

void SoftwareGpu::execute_gp0() {
    if (packet_.empty()) return;
    const auto opcode = opcode_of(packet_[0]);
    const auto color = packet_[0] & 0x00FFFFFFu;

    if (opcode == 0x00 || opcode == 0x01 || opcode == 0x03 ||
        opcode == 0x1F) {
        // nop, texture-cache clear, timing control, IRQ request.
        return;
    }

    if (opcode == 0x02) {
        fill_rectangle(color, packet_[1], packet_[2]);
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x20 && opcode <= 0x23) {
        const auto color555 = to_bgr555(color);
        draw_triangle(
            coordinate_x(packet_[1]) + draw_offset_x_,
            coordinate_y(packet_[1]) + draw_offset_y_,
            coordinate_x(packet_[2]) + draw_offset_x_,
            coordinate_y(packet_[2]) + draw_offset_y_,
            coordinate_x(packet_[3]) + draw_offset_x_,
            coordinate_y(packet_[3]) + draw_offset_y_,
            color555);
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x30 && opcode <= 0x33) {
        // Gouraud three-point polygon: colour0,xy0,xy1,colour1,xy2,colour2.
        draw_gouraud_triangle(
            coordinate_x(packet_[1]) + draw_offset_x_,
            coordinate_y(packet_[1]) + draw_offset_y_, to_bgr555(color),
            coordinate_x(packet_[2]) + draw_offset_x_,
            coordinate_y(packet_[2]) + draw_offset_y_,
            to_bgr555(packet_[3]),
            coordinate_x(packet_[4]) + draw_offset_x_,
            coordinate_y(packet_[4]) + draw_offset_y_,
            to_bgr555(packet_[5]));
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x38 && opcode <= 0x3B) {
        // Gouraud four-point polygon, drawn as two shaded triangles.
        const auto c0 = to_bgr555(color);
        const auto c1 = to_bgr555(packet_[3]);
        const auto c2 = to_bgr555(packet_[5]);
        const auto c3 = to_bgr555(packet_[7]);
        draw_gouraud_triangle(
            coordinate_x(packet_[1]) + draw_offset_x_,
            coordinate_y(packet_[1]) + draw_offset_y_, c0,
            coordinate_x(packet_[2]) + draw_offset_x_,
            coordinate_y(packet_[2]) + draw_offset_y_, c1,
            coordinate_x(packet_[4]) + draw_offset_x_,
            coordinate_y(packet_[4]) + draw_offset_y_, c2);
        draw_gouraud_triangle(
            coordinate_x(packet_[2]) + draw_offset_x_,
            coordinate_y(packet_[2]) + draw_offset_y_, c1,
            coordinate_x(packet_[4]) + draw_offset_x_,
            coordinate_y(packet_[4]) + draw_offset_y_, c2,
            coordinate_x(packet_[6]) + draw_offset_x_,
            coordinate_y(packet_[6]) + draw_offset_y_, c3);
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x24 && opcode <= 0x27) {
        const bool semi_transparent = (opcode & 0x02u) != 0;
        const bool raw = (opcode & 0x01u) != 0;
        const auto clut = packet_[2] >> 16;
        const auto texpage = packet_[4] >> 16;
        texpage_ = texpage & 0x7FFu;
        const TexturedVertex first{
            coordinate_x(packet_[1]) + draw_offset_x_,
            coordinate_y(packet_[1]) + draw_offset_y_,
            static_cast<std::uint8_t>(packet_[2] & 0xFFu),
            static_cast<std::uint8_t>((packet_[2] >> 8) & 0xFFu)};
        const TexturedVertex second{
            coordinate_x(packet_[3]) + draw_offset_x_,
            coordinate_y(packet_[3]) + draw_offset_y_,
            static_cast<std::uint8_t>(packet_[4] & 0xFFu),
            static_cast<std::uint8_t>((packet_[4] >> 8) & 0xFFu)};
        const TexturedVertex third{
            coordinate_x(packet_[5]) + draw_offset_x_,
            coordinate_y(packet_[5]) + draw_offset_y_,
            static_cast<std::uint8_t>(packet_[6] & 0xFFu),
            static_cast<std::uint8_t>((packet_[6] >> 8) & 0xFFu)};
        draw_textured_triangle(first, second, third,
                               raw ? 0x808080u : color, clut, texpage,
                               semi_transparent);
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x2C && opcode <= 0x2F) {
        const bool semi_transparent = (opcode & 0x02u) != 0;
        const bool raw = (opcode & 0x01u) != 0;
        const auto clut = packet_[2] >> 16;
        const auto texpage = packet_[4] >> 16;
        texpage_ = texpage & 0x7FFu;
        const auto modulation = raw ? 0x808080u : color;
        const TexturedVertex corners[4] = {
            {coordinate_x(packet_[1]) + draw_offset_x_,
             coordinate_y(packet_[1]) + draw_offset_y_,
             static_cast<std::uint8_t>(packet_[2] & 0xFFu),
             static_cast<std::uint8_t>((packet_[2] >> 8) & 0xFFu)},
            {coordinate_x(packet_[3]) + draw_offset_x_,
             coordinate_y(packet_[3]) + draw_offset_y_,
             static_cast<std::uint8_t>(packet_[4] & 0xFFu),
             static_cast<std::uint8_t>((packet_[4] >> 8) & 0xFFu)},
            {coordinate_x(packet_[5]) + draw_offset_x_,
             coordinate_y(packet_[5]) + draw_offset_y_,
             static_cast<std::uint8_t>(packet_[6] & 0xFFu),
             static_cast<std::uint8_t>((packet_[6] >> 8) & 0xFFu)},
            {coordinate_x(packet_[7]) + draw_offset_x_,
             coordinate_y(packet_[7]) + draw_offset_y_,
             static_cast<std::uint8_t>(packet_[8] & 0xFFu),
             static_cast<std::uint8_t>((packet_[8] >> 8) & 0xFFu)}};
        draw_textured_triangle(corners[0], corners[1], corners[2], modulation,
                               clut, texpage, semi_transparent);
        draw_textured_triangle(corners[0], corners[2], corners[3], modulation,
                               clut, texpage, semi_transparent);
        ++rendered_primitives_;
        return;
    }

    const bool textured_rectangle =
        (opcode >= 0x64 && opcode <= 0x67) ||
        (opcode >= 0x6C && opcode <= 0x6F) ||
        (opcode >= 0x74 && opcode <= 0x77) ||
        (opcode >= 0x7C && opcode <= 0x7F);
    if (textured_rectangle) {
        const bool semi_transparent = (opcode & 0x02u) != 0;
        const bool raw = (opcode & 0x01u) != 0;
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        if (opcode >= 0x64 && opcode <= 0x67) {
            width = rect_width(packet_[3]);
            height = rect_height(packet_[3]);
        } else if (opcode >= 0x74 && opcode <= 0x77) {
            width = 8;
            height = 8;
        } else if (opcode >= 0x7C && opcode <= 0x7F) {
            width = 16;
            height = 16;
        }
        draw_textured_rectangle(raw ? 0x808080u : color, packet_[1],
                                packet_[2] & 0xFFFFu, width, height,
                                packet_[2] >> 16, texpage_,
                                semi_transparent);
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x60 && opcode <= 0x7F) {
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        if (opcode <= 0x63) {
            width = rect_width(packet_[2]);
            height = rect_height(packet_[2]);
        } else if (opcode >= 0x70 && opcode <= 0x73) {
            width = 8;
            height = 8;
        } else if (opcode >= 0x78) {
            width = 16;
            height = 16;
        }
        draw_rectangle(color, packet_[1], width, height);
        ++rendered_primitives_;
        return;
    }

    if (opcode >= 0x80 && opcode <= 0x9F) {
        copy_vram(packet_[1], packet_[2], packet_[3]);
        return;
    }

    if (is_upload(opcode)) {
        upload_image();
        return;
    }

    if (opcode >= 0xC0 && opcode <= 0xDF) {
        begin_download(packet_[1], packet_[2]);
        return;
    }

    if (opcode >= 0xE0 && opcode <= 0xE6) {
        execute_environment(opcode);
        return;
    }

    record_unsupported(opcode);
}

void SoftwareGpu::execute_environment(std::uint8_t opcode) {
    const auto value = packet_[0];
    switch (opcode) {
        case 0xE1:
            texpage_ = value & 0x7FFu;
            break;
        case 0xE2:
            texture_window_ = value & 0xFFFFFu;
            break;
        case 0xE3:
            draw_area_left_ = static_cast<std::int32_t>(value & 0x3FFu);
            draw_area_top_ =
                static_cast<std::int32_t>((value >> 10) & 0x1FFu);
            break;
        case 0xE4:
            draw_area_right_ = static_cast<std::int32_t>(value & 0x3FFu);
            draw_area_bottom_ =
                static_cast<std::int32_t>((value >> 10) & 0x1FFu);
            break;
        case 0xE5:
            draw_offset_x_ = signed_offset(value);
            draw_offset_y_ = signed_offset(value >> 11);
            break;
        case 0xE6:
            mask_bit_setting_ = value & 0x3u;
            break;
        default:
            record_unsupported(opcode);
            break;
    }
}

void SoftwareGpu::fill_rectangle(std::uint32_t rgb,
                                 std::uint32_t position,
                                 std::uint32_t size) {
    ++fill_rectangles_;
    const auto color = to_bgr555(rgb);
    // Bring-up: show the first fills and a few later ones, so it is visible
    // whether a rectangle lands where the guest asked for it.
    if (std::getenv("R6_TRACE_FILL") != nullptr) {
        static int shown = 0;
        ++shown;
        if (shown <= 24 || (shown % 4000) == 0) {
            std::printf("FILL %d pos=%08X size=%08X color=%06X "
                        "area=%d,%d-%d,%d\n",
                        shown, position, size, rgb & 0xFFFFFFu,
                        draw_area_left_, draw_area_top_, draw_area_right_,
                        draw_area_bottom_);
        }
    }
    // Hardware rounds the start down and the width up to 16-pixel units, and
    // the fill is not affected by the drawing area or drawing offset.
    const auto start_x =
        static_cast<std::int32_t>(position & 0x3FFu & ~0xFu);
    const auto start_y = static_cast<std::int32_t>((position >> 16) & 0x1FFu);
    const auto width = (rect_width(size) + 15u) & ~15u;
    const auto height = rect_height(size);

    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            store_pixel(start_x + static_cast<std::int32_t>(column),
                        start_y + static_cast<std::int32_t>(row), color);
        }
    }
}

void SoftwareGpu::draw_rectangle(std::uint32_t rgb,
                                 std::uint32_t position,
                                 std::uint32_t width,
                                 std::uint32_t height) {
    const auto color = to_bgr555(rgb);
    const auto start_x = coordinate_x(position) + draw_offset_x_;
    const auto start_y = coordinate_y(position) + draw_offset_y_;
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            write_pixel(start_x + static_cast<std::int32_t>(column),
                        start_y + static_cast<std::int32_t>(row), color);
        }
    }
}

void SoftwareGpu::draw_triangle(std::int32_t x0,
                                std::int32_t y0,
                                std::int32_t x1,
                                std::int32_t y1,
                                std::int32_t x2,
                                std::int32_t y2,
                                std::uint16_t color) {
    const auto edge = [](std::int32_t ax, std::int32_t ay, std::int32_t bx,
                         std::int32_t by, std::int32_t cx, std::int32_t cy) {
        return static_cast<std::int64_t>(bx - ax) *
                   static_cast<std::int64_t>(cy - ay) -
               static_cast<std::int64_t>(by - ay) *
                   static_cast<std::int64_t>(cx - ax);
    };

    const auto area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    const auto min_x = std::max<std::int32_t>(
        draw_area_left_, std::min({x0, x1, x2}));
    const auto max_x = std::min<std::int32_t>(
        draw_area_right_, std::max({x0, x1, x2}));
    const auto min_y = std::max<std::int32_t>(
        draw_area_top_, std::min({y0, y1, y2}));
    const auto max_y = std::min<std::int32_t>(
        draw_area_bottom_, std::max({y0, y1, y2}));
    if (min_x > max_x || min_y > max_y) return;

    const bool counter_clockwise = area > 0;
    for (std::int32_t y = min_y; y <= max_y; ++y) {
        for (std::int32_t x = min_x; x <= max_x; ++x) {
            const auto w0 = edge(x1, y1, x2, y2, x, y);
            const auto w1 = edge(x2, y2, x0, y0, x, y);
            const auto w2 = edge(x0, y0, x1, y1, x, y);
            const bool inside =
                counter_clockwise
                    ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                    : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) store_pixel(x, y, color);
        }
    }
}

void SoftwareGpu::draw_gouraud_triangle(std::int32_t x0,
                                        std::int32_t y0,
                                        std::uint16_t color0,
                                        std::int32_t x1,
                                        std::int32_t y1,
                                        std::uint16_t color1,
                                        std::int32_t x2,
                                        std::int32_t y2,
                                        std::uint16_t color2) {
    const auto edge = [](std::int32_t ax, std::int32_t ay, std::int32_t bx,
                         std::int32_t by, std::int32_t cx, std::int32_t cy) {
        return static_cast<std::int64_t>(bx - ax) *
                   static_cast<std::int64_t>(cy - ay) -
               static_cast<std::int64_t>(by - ay) *
                   static_cast<std::int64_t>(cx - ax);
    };

    const auto area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    const auto min_x = std::max<std::int32_t>(
        draw_area_left_, std::min({x0, x1, x2}));
    const auto max_x = std::min<std::int32_t>(
        draw_area_right_, std::max({x0, x1, x2}));
    const auto min_y = std::max<std::int32_t>(
        draw_area_top_, std::min({y0, y1, y2}));
    const auto max_y = std::min<std::int32_t>(
        draw_area_bottom_, std::max({y0, y1, y2}));
    if (min_x > max_x || min_y > max_y) return;

    const auto channel = [](std::uint16_t color, int shift) {
        return static_cast<std::int64_t>((color >> shift) & 0x1Fu);
    };
    const std::int64_t red[3] = {channel(color0, 0), channel(color1, 0),
                                 channel(color2, 0)};
    const std::int64_t green[3] = {channel(color0, 5), channel(color1, 5),
                                   channel(color2, 5)};
    const std::int64_t blue[3] = {channel(color0, 10), channel(color1, 10),
                                  channel(color2, 10)};

    const bool counter_clockwise = area > 0;
    for (std::int32_t y = min_y; y <= max_y; ++y) {
        for (std::int32_t x = min_x; x <= max_x; ++x) {
            const auto w0 = edge(x1, y1, x2, y2, x, y);
            const auto w1 = edge(x2, y2, x0, y0, x, y);
            const auto w2 = edge(x0, y0, x1, y1, x, y);
            const bool inside =
                counter_clockwise
                    ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                    : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;
            // The barycentric weights sum to the signed area, so the same
            // division interpolates both winding orders.
            const auto mix = [&](const std::int64_t (&values)[3]) {
                const auto total = w0 * values[0] + w1 * values[1] +
                                   w2 * values[2];
                return static_cast<std::uint16_t>(
                    std::clamp<std::int64_t>(total / area, 0, 31));
            };
            const auto pixel = static_cast<std::uint16_t>(
                mix(red) | (mix(green) << 5) | (mix(blue) << 10));
            store_pixel(x, y, pixel);
        }
    }
}

void SoftwareGpu::draw_textured_triangle(const TexturedVertex& first,
                                         const TexturedVertex& second,
                                         const TexturedVertex& third,
                                         std::uint32_t modulation,
                                         std::uint32_t clut,
                                         std::uint32_t texpage,
                                         bool semi_transparent) {
    const auto edge = [](std::int32_t ax, std::int32_t ay, std::int32_t bx,
                         std::int32_t by, std::int32_t cx, std::int32_t cy) {
        return static_cast<std::int64_t>(bx - ax) *
                   static_cast<std::int64_t>(cy - ay) -
               static_cast<std::int64_t>(by - ay) *
                   static_cast<std::int64_t>(cx - ax);
    };

    const auto area = edge(first.x, first.y, second.x, second.y, third.x,
                           third.y);
    if (area == 0) return;

    const auto min_x = std::max<std::int32_t>(
        draw_area_left_, std::min({first.x, second.x, third.x}));
    const auto max_x = std::min<std::int32_t>(
        draw_area_right_, std::max({first.x, second.x, third.x}));
    const auto min_y = std::max<std::int32_t>(
        draw_area_top_, std::min({first.y, second.y, third.y}));
    const auto max_y = std::min<std::int32_t>(
        draw_area_bottom_, std::max({first.y, second.y, third.y}));
    if (min_x > max_x || min_y > max_y) return;

    const bool counter_clockwise = area > 0;
    for (std::int32_t y = min_y; y <= max_y; ++y) {
        for (std::int32_t x = min_x; x <= max_x; ++x) {
            const auto w0 = edge(second.x, second.y, third.x, third.y, x, y);
            const auto w1 = edge(third.x, third.y, first.x, first.y, x, y);
            const auto w2 = edge(first.x, first.y, second.x, second.y, x, y);
            const bool inside =
                counter_clockwise
                    ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                    : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;

            // Affine texture mapping, matching the hardware (no perspective).
            const auto u =
                (w0 * first.u + w1 * second.u + w2 * third.u) / area;
            const auto v =
                (w0 * first.v + w1 * second.v + w2 * third.v) / area;
            const auto texel = sample_texture(
                static_cast<std::uint32_t>(u),
                static_cast<std::uint32_t>(v), clut, texpage);
            plot_textured(x, y, texel, modulation, texpage, semi_transparent);
        }
    }
}

void SoftwareGpu::draw_textured_rectangle(std::uint32_t modulation,
                                          std::uint32_t position,
                                          std::uint32_t texcoord,
                                          std::uint32_t width,
                                          std::uint32_t height,
                                          std::uint32_t clut,
                                          std::uint32_t texpage,
                                          bool semi_transparent) {
    const auto start_x = coordinate_x(position) + draw_offset_x_;
    const auto start_y = coordinate_y(position) + draw_offset_y_;
    const auto base_u = texcoord & 0xFFu;
    const auto base_v = (texcoord >> 8) & 0xFFu;

    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            const auto texel = sample_texture(base_u + column, base_v + row,
                                              clut, texpage);
            plot_textured(start_x + static_cast<std::int32_t>(column),
                          start_y + static_cast<std::int32_t>(row), texel,
                          modulation, texpage, semi_transparent);
        }
    }
}

std::uint32_t SoftwareGpu::texture_window_coordinate(
    std::uint32_t coordinate,
    bool horizontal) const {
    const auto mask = horizontal ? (texture_window_ & 0x1Fu)
                                 : ((texture_window_ >> 5) & 0x1Fu);
    const auto offset = horizontal ? ((texture_window_ >> 10) & 0x1Fu)
                                   : ((texture_window_ >> 15) & 0x1Fu);
    // Texcoord = (Texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
    return (coordinate & ~(mask * 8u)) | ((offset & mask) * 8u);
}

std::uint16_t SoftwareGpu::sample_texture(std::uint32_t u,
                                          std::uint32_t v,
                                          std::uint32_t clut,
                                          std::uint32_t texpage) const {
    u = texture_window_coordinate(u, true) & 0xFFu;
    v = texture_window_coordinate(v, false) & 0xFFu;

    const auto page_x = (texpage & 0xFu) * 64u;
    const auto page_y = ((texpage >> 4) & 0x1u) * 256u;
    const auto depth = (texpage >> 7) & 0x3u;
    const auto clut_x = (clut & 0x3Fu) * 16u;
    const auto clut_y = (clut >> 6) & 0x1FFu;

    if (depth == 0) {
        const auto word = read_vram(page_x + (u >> 2), page_y + v);
        const auto index = (word >> ((u & 3u) * 4u)) & 0xFu;
        return read_vram(clut_x + index, clut_y);
    }
    if (depth == 1) {
        const auto word = read_vram(page_x + (u >> 1), page_y + v);
        const auto index = (word >> ((u & 1u) * 8u)) & 0xFFu;
        return read_vram(clut_x + index, clut_y);
    }
    return read_vram(page_x + u, page_y + v);
}

void SoftwareGpu::plot_textured(std::int32_t x,
                                std::int32_t y,
                                std::uint16_t texel,
                                std::uint32_t modulation,
                                std::uint32_t texpage,
                                bool semi_transparent) {
    // A zero texel is fully transparent and leaves the destination untouched.
    if (texel == 0x0000u) return;
    if (x < draw_area_left_ || x > draw_area_right_) return;
    if (y < draw_area_top_ || y > draw_area_bottom_) return;

    auto pixel = modulate_texel(static_cast<std::uint16_t>(texel & 0x7FFFu),
                                modulation);

    // Bit 15 of the texel (or of its CLUT entry) marks the pixel as
    // semi-transparent; the blend mode comes from the texture page.
    if (semi_transparent && (texel & 0x8000u) != 0) {
        const auto back = read_vram(static_cast<std::uint32_t>(x),
                                    static_cast<std::uint32_t>(y));
        pixel = blend_texel(back, pixel, (texpage >> 5) & 0x3u);
    }
    store_pixel(x, y, pixel);
}

void SoftwareGpu::copy_vram(std::uint32_t source,
                            std::uint32_t destination,
                            std::uint32_t size) {
    const auto width = rect_width(size);
    const auto height = rect_height(size);
    if (width == 0 || height == 0) return;

    const auto source_x = coordinate_x(source);
    const auto source_y = coordinate_y(source);
    const auto destination_x = coordinate_x(destination);
    const auto destination_y = coordinate_y(destination);

    // Copy through a staging buffer so overlapping regions behave like the
    // hardware's scanline-sized transfer.
    std::vector<std::uint16_t> staging(
        static_cast<std::size_t>(width) * height, 0);
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            staging[static_cast<std::size_t>(row) * width + column] =
                read_vram(static_cast<std::uint32_t>(source_x) + column,
                          static_cast<std::uint32_t>(source_y) + row);
        }
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            store_pixel(destination_x + static_cast<std::int32_t>(column),
                        destination_y + static_cast<std::int32_t>(row),
                        staging[static_cast<std::size_t>(row) * width + column]);
        }
    }
    ++rendered_primitives_;
}

void SoftwareGpu::upload_image() {
    if (packet_.size() < 3) {
        record_unsupported(opcode_of(packet_[0]));
        return;
    }
    const auto start_x = coordinate_x(packet_[1]);
    const auto start_y = coordinate_y(packet_[1]);
    const auto width = rect_width(packet_[2]);
    const auto height = rect_height(packet_[2]);
    if (width == 0 || height == 0) return;

    const auto total = static_cast<std::size_t>(width) * height;
    std::size_t pixel = 0;
    for (std::size_t index = 3; index < packet_.size() && pixel < total;
         ++index) {
        store_pixel(start_x + static_cast<std::int32_t>(pixel % width),
                    start_y + static_cast<std::int32_t>(pixel / width),
                    static_cast<std::uint16_t>(packet_[index] & 0xFFFFu));
        ++pixel;
        if (pixel >= total) break;
        store_pixel(start_x + static_cast<std::int32_t>(pixel % width),
                    start_y + static_cast<std::int32_t>(pixel / width),
                    static_cast<std::uint16_t>(packet_[index] >> 16));
        ++pixel;
    }
}

void SoftwareGpu::begin_download(std::uint32_t position, std::uint32_t size) {
    const auto width = rect_width(size);
    const auto height = rect_height(size);
    const auto start_x = coordinate_x(position);
    const auto start_y = coordinate_y(position);

    download_words_.clear();
    download_index_ = 0;
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t column = 0; column < width; ++column) {
            download_words_.push_back(
                read_vram(static_cast<std::uint32_t>(start_x) + column,
                          static_cast<std::uint32_t>(start_y) + row));
        }
    }
}

void SoftwareGpu::store_pixel(std::int32_t x,
                              std::int32_t y,
                              std::uint16_t color) {
    if (x < 0 || y < 0) return;
    if (x >= static_cast<std::int32_t>(kVramWidth) ||
        y >= static_cast<std::int32_t>(kVramHeight)) {
        return;
    }
    vram_[static_cast<std::size_t>(y) * kVramWidth +
          static_cast<std::size_t>(x)] = color;
}

void SoftwareGpu::write_pixel(std::int32_t x,
                              std::int32_t y,
                              std::uint16_t color) {
    if (x < draw_area_left_ || x > draw_area_right_) return;
    if (y < draw_area_top_ || y > draw_area_bottom_) return;
    store_pixel(x, y, color);
}

std::uint16_t SoftwareGpu::read_vram(std::uint32_t x,
                                     std::uint32_t y) const {
    if (x >= kVramWidth || y >= kVramHeight) return 0;
    return vram_[static_cast<std::size_t>(y) * kVramWidth +
                 static_cast<std::size_t>(x)];
}

std::uint32_t SoftwareGpu::read_gp0_data() {
    if (download_index_ >= download_words_.size()) return 0;
    std::uint32_t value = download_words_[download_index_++];
    if (download_index_ < download_words_.size()) {
        value |= static_cast<std::uint32_t>(download_words_[download_index_++])
                 << 16;
    }
    return value;
}

std::uint32_t SoftwareGpu::status() const {
    // Bits 26-28 report command, DMA and read readiness; the decoder is never
    // busy, so they stay set. Bit 23 is the display-disable flag.
    std::uint32_t value = 0x1C000000u;
    value |= (dma_direction_ & 0x3u) << 29;
    value |= texpage_ & 0x7FFu;
    value |= (mask_bit_setting_ & 0x3u) << 11;
    if (!display_enabled_) value |= 1u << 23;
    return value;
}

void SoftwareGpu::submit_gp1(std::uint32_t word) {
    const auto command = static_cast<std::uint8_t>(word >> 24);
    last_gp1_command_ = command;
    ++gp1_writes_;
    switch (command) {
        case 0x00:  // reset
            reset_registers();
            std::fill(vram_.begin(), vram_.end(),
                      static_cast<std::uint16_t>(0));
            packet_.clear();
            expected_words_ = 0;
            download_words_.clear();
            download_index_ = 0;
            break;
        case 0x01:  // reset command buffer
            packet_.clear();
            expected_words_ = 0;
            break;
        case 0x02:  // acknowledge IRQ
            break;
        case 0x03:  // display enable
            // GP1(03h) bit 0 is DMSK: 0 = display on, 1 = display off.
            display_enabled_ = (word & 1u) == 0;
            ++display_enable_writes_;
            last_display_enable_word_ = word;
            break;
        case 0x04:  // DMA direction
            dma_direction_ = word & 0x3u;
            break;
        case 0x05:  // display area start
            display_area_x_ = static_cast<std::int32_t>(word & 0x3FFu);
            display_area_y_ = static_cast<std::int32_t>((word >> 10) & 0x1FFu);
            break;
        case 0x06:  // horizontal display range
        case 0x07:  // vertical display range
            break;
        case 0x08:  // display mode
            display_mode_ = word & 0xFFFFu;
            break;
        default:
            break;
    }
}

std::uint32_t SoftwareGpu::filled_vram_pixels() const {
    std::uint32_t filled = 0;
    for (const auto pixel : vram_) {
        if (pixel != 0) ++filled;
    }
    return filled;
}

bool SoftwareGpu::filled_vram_bounds(std::uint32_t& left, std::uint32_t& top,
                                     std::uint32_t& right,
                                     std::uint32_t& bottom) const {
    bool found = false;
    for (std::uint32_t y = 0; y < kVramHeight; ++y) {
        for (std::uint32_t x = 0; x < kVramWidth; ++x) {
            if (vram_[static_cast<std::size_t>(y) * kVramWidth + x] == 0) {
                continue;
            }
            if (!found) {
                left = right = x;
                top = bottom = y;
                found = true;
                continue;
            }
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    return found;
}

std::vector<std::uint32_t> SoftwareGpu::display_frame() const {
    std::vector<std::uint32_t> frame(
        static_cast<std::size_t>(kDisplayWidth) * kDisplayHeight, 0xFF000000u);
    // Display mode bit 4 selects a 24-bit framebuffer. In that mode the GPU
    // still receives the image through 16-bit uploads, but the bytes are the
    // three colour bytes of each pixel in order, so a line spans 960 bytes and
    // the next line starts one VRAM row (2048 bytes) later. Decoding such a
    // buffer as 15-bit turns every colour into a three-pixel cycle, which is
    // what the movie previously looked like.
    if ((display_mode_ & 0x10u) != 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(vram_.data());
        const auto limit = vram_.size() * sizeof(std::uint16_t);
        for (std::uint32_t y = 0; y < kDisplayHeight; ++y) {
            const auto source_y = display_area_y_ + static_cast<std::int32_t>(y);
            if (source_y < 0 ||
                source_y >= static_cast<std::int32_t>(kVramHeight)) {
                continue;
            }
            const std::size_t line =
                static_cast<std::size_t>(source_y) * kVramWidth *
                    sizeof(std::uint16_t) +
                static_cast<std::size_t>(display_area_x_) *
                    sizeof(std::uint16_t);
            for (std::uint32_t x = 0; x < kDisplayWidth; ++x) {
                const std::size_t offset = line + static_cast<std::size_t>(x) * 3;
                if (offset + 2 >= limit) continue;
                frame[static_cast<std::size_t>(y) * kDisplayWidth + x] =
                    0xFF000000u |
                    (static_cast<std::uint32_t>(bytes[offset]) << 16) |
                    (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                    static_cast<std::uint32_t>(bytes[offset + 2]);
            }
        }
        return frame;
    }
    for (std::uint32_t y = 0; y < kDisplayHeight; ++y) {
        const auto source_y = display_area_y_ + static_cast<std::int32_t>(y);
        if (source_y < 0 ||
            source_y >= static_cast<std::int32_t>(kVramHeight)) {
            continue;
        }
        for (std::uint32_t x = 0; x < kDisplayWidth; ++x) {
            const auto source_x = display_area_x_ + static_cast<std::int32_t>(x);
            if (source_x < 0 ||
                source_x >= static_cast<std::int32_t>(kVramWidth)) {
                continue;
            }
            frame[static_cast<std::size_t>(y) * kDisplayWidth + x] =
                to_xrgb(vram_[static_cast<std::size_t>(source_y) * kVramWidth +
                             static_cast<std::size_t>(source_x)]);
        }
    }
    return frame;
}

std::uint32_t SoftwareGpu::unsupported_commands(std::uint8_t opcode) const {
    const auto found = unsupported_by_opcode_.find(opcode);
    return found == unsupported_by_opcode_.end() ? 0u : found->second;
}

void SoftwareGpu::record_unsupported(std::uint8_t opcode) {
    ++unsupported_commands_;
    ++unsupported_by_opcode_[opcode];
    last_unsupported_opcode_ = opcode;
}

}  // namespace r6
