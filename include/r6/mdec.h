#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace r6 {

// Macroblock decoder: turns the compressed video stream into pixels.
//
// The movie player is the only user here, so the model covers what an STR
// player drives: the command/parameter register pair, the run-length decode
// with the quant tables, the inverse DCT and the 4:2:0 macroblock that the
// colour modes produce. Without it the player's DMA1 channel never gets a
// request, the driver finds the channel unarmed and resets the drive, which is
// why no frame was ever decoded.
class MdecDevice {
public:
    MdecDevice() { reset(); }

    void reset() {
        input_.clear();
        output_.clear();
        output_index_ = 0;
        command_ = 0;
        param_target_ = 0;
        half_index_ = 0;
        half_end_ = 0;
        colour_ = false;
        depth_ = 0;
        signed_ = true;
        bit15_ = false;
        enable_in_ = false;
        enable_out_ = false;
        macroblocks_ = 0;
        words_in_ = 0;
        words_out_ = 0;
        iq_y_.fill(1);
        iq_uv_.fill(1);
    }

    // 1F801820h write: a command word, or a parameter word while a command is
    // still collecting its parameters.
    void write_command(std::uint32_t value) {
        if (command_ != 0 && input_.size() < param_target_) {
            push_word(value);
            return;
        }
        const auto opcode = value >> 29;
        switch (opcode) {
            case 0:
                reset();
                return;
            case 1:
                // Decode. The low half is the compressed size in words; a
                // zero there means the whole 65536-word window.
                command_ = 1;
                depth_ = (value >> 27) & 3u;
                signed_ = (value & (1u << 26)) != 0;
                bit15_ = (value & (1u << 25)) != 0;
                param_target_ = value & 0xFFFFu;
                if (param_target_ == 0) param_target_ = 0x10000u;
                input_.clear();
                return;
            case 2:
                // Set quant tables: 64 bytes each, chroma only when bit 0 is
                // set. Parameters arrive as words, so 16 words per table.
                command_ = 2;
                colour_ = (value & 1u) != 0;
                param_target_ = colour_ ? 32u : 16u;
                input_.clear();
                return;
            case 3:
                // Set scale table: 64 signed halfwords.
                command_ = 3;
                param_target_ = 32u;
                input_.clear();
                return;
            default:
                return;
        }
    }

    // DMA0 (RAM to MDEC) and 1F801820h parameter writes land here. A decode
    // emits every macroblock whose bits have arrived instead of waiting for
    // the whole stream: the player feeds a few words at a time and waits for
    // the output before sending more, so a decode held back until the last
    // parameter word would never produce anything.
    void push_word(std::uint32_t value) {
        if (command_ == 0) return;
        if (input_.size() >= param_target_) return;
        input_.push_back(value);
        ++words_in_;
        if (command_ == 1) {
            decode_available();
            if (input_.size() >= param_target_) {
                command_ = 0;
                input_.clear();
                half_index_ = 0;
                half_end_ = 0;
            }
            return;
        }
        if (input_.size() >= param_target_) execute();
    }

    // 1F801820h read and DMA1 (MDEC to RAM).
    std::uint32_t read_word() {
        if (output_index_ >= output_.size()) {
            output_.clear();
            output_index_ = 0;
            return 0;
        }
        return output_[output_index_++];
    }

    // 1F801824h write: bit 31 resets, 30 enables data-in requests, 29 data-out.
    void write_control(std::uint32_t value) {
        if ((value & 0x80000000u) != 0) {
            reset();
            return;
        }
        enable_in_ = (value & 0x40000000u) != 0;
        enable_out_ = (value & 0x20000000u) != 0;
    }

    std::uint32_t status() const {
        std::uint32_t value = 0;
        if (output_index_ >= output_.size()) value |= 0x80000000u;
        if (command_ != 0) {
            value |= 0x20000000u;  // busy with a command
            if (enable_in_ && input_.size() < param_target_) {
                value |= 0x10000000u;  // data-in request
            }
        }
        if (enable_out_ && output_index_ < output_.size()) {
            value |= 0x08000000u;  // data-out request
        }
        value |= (depth_ & 3u) << 25;
        if (signed_) value |= 0x01000000u;
        if (bit15_) value |= 0x00800000u;
        // 4 is "Y" for monochrome and the first luma block otherwise.
        value |= 4u << 16;
        if (command_ == 0) {
            value |= 0xFFFFu;  // no parameter words outstanding
        } else {
            const auto left = param_target_ - input_.size();
            value |= static_cast<std::uint32_t>(left > 0 ? left - 1 : 0);
        }
        return value;
    }

    bool output_pending() const { return output_index_ < output_.size(); }
    std::size_t pending_words() const {
        return output_index_ < output_.size() ? output_.size() - output_index_
                                              : 0;
    }
    std::uint32_t depth() const { return depth_; }

    // Bring-up counters: "the player never sent a command" and "the decode
    // produced nothing" look the same from the DMA side otherwise.
    std::uint64_t macroblocks() const { return macroblocks_; }
    std::uint64_t words_in() const { return words_in_; }
    std::uint64_t words_out() const { return words_out_; }

private:
    using Block = std::array<int, 64>;

    void execute() {
        switch (command_) {
            case 2: {
                const auto bytes = colour_ ? 128 : 64;
                for (int index = 0; index < bytes; ++index) {
                    const auto word = input_[static_cast<std::size_t>(index) / 4];
                    const auto byte = static_cast<std::uint8_t>(
                        word >> ((index % 4) * 8));
                    if (index < 64) {
                        iq_y_[static_cast<std::size_t>(index)] = byte;
                    } else {
                        iq_uv_[static_cast<std::size_t>(index - 64)] = byte;
                    }
                }
                break;
            }
            case 3: {
                for (int index = 0; index < 64; ++index) {
                    const auto word = input_[static_cast<std::size_t>(index) / 2];
                    const auto half = static_cast<std::uint16_t>(
                        (index % 2) == 0 ? word : (word >> 16));
                    scale_[static_cast<std::size_t>(index)] =
                        static_cast<std::int16_t>(half);
                }
                break;
            }
            case 1:
                decode_available();
                break;
            default:
                break;
        }
        command_ = 0;
        input_.clear();
        param_target_ = 0;
    }

    // Decodes from the current position as far as the buffered words allow and
    // leaves the read position at the last block boundary, so the rest of a
    // block is retried when the player's next transfer arrives.
    void decode_available() {
        half_end_ = input_.size() * 2;
        for (;;) {
            const auto start = half_index_;
            if (half_index_ >= half_end_) break;
            if (depth_ <= 1) {
                Block luma{};
                if (!decode_block(iq_y_, luma)) {
                    half_index_ = start;
                    break;
                }
                emit_mono(luma);
            } else {
                Block cr{};
                Block cb{};
                std::array<Block, 4> luma{};
                bool complete = decode_block(iq_uv_, cr) &&
                                decode_block(iq_uv_, cb);
                for (int quadrant = 0; complete && quadrant < 4; ++quadrant) {
                    complete = decode_block(iq_y_, luma[static_cast<std::size_t>(
                                                quadrant)]);
                }
                if (!complete) {
                    half_index_ = start;
                    break;
                }
                emit_colour(cr, cb, luma);
            }
            ++macroblocks_;
        }
    }

    std::uint16_t next_half() {
        const auto index = half_index_++;
        const auto word = input_[index / 2];
        return static_cast<std::uint16_t>((index & 1) == 0 ? word
                                                          : (word >> 16));
    }

    static int signed10(std::uint16_t value) {
        return static_cast<int>((value & 0x3FFu) ^ 0x200u) - 0x200;
    }

    // Run-length decode of one block: DC first, then (run, level) pairs until
    // the end code (run 3Fh) takes the scan past coefficient 63. Returns false
    // when the buffered words run out first, so the caller can retry the block
    // once more of the stream has arrived.
    bool decode_block(const std::array<std::uint8_t, 64>& quant, Block& block) {
        block.fill(0);
        std::uint16_t code = 0xFE00u;
        // FE00h is the padding code; skip any that precede a real block.
        do {
            if (half_index_ >= half_end_) return false;
            code = next_half();
        } while (code == 0xFE00u);

        const int q_scale = (code >> 10) & 0x3F;
        int k = 0;
        int value = signed10(code) * quant[0];
        for (;;) {
            if (q_scale == 0) {
                // Unscaled mode: the code carries the level directly and the
                // coefficients stay in scan order.
                value = signed10(code) * 2;
            }
            value = std::clamp(value, -0x400, 0x3FF);
            block[static_cast<std::size_t>(q_scale > 0 ? kZigZag[k] : k)] =
                value;
            if (half_index_ >= half_end_) return false;
            code = next_half();
            k += ((code >> 10) & 0x3F) + 1;
            if (k > 63) break;
            // The hardware reads the quant entry for the *next* scan position
            // here, past the end of the table when the block runs long.
            // (Written out rather than std::min, which the Windows headers
            // define as a macro.)
            const auto entry_index = k < 63 ? k : 63;
            const auto entry = quant[static_cast<std::size_t>(entry_index)];
            value = (signed10(code) * entry * q_scale + 4) / 8;
        }
        idct(block);
        return true;
    }

    // Separable IDCT with the 1/4 scaling the hardware applies: a block whose
    // only coefficient is the DC term comes out flat at DC/8.
    static void idct(Block& block) {
        static const auto cosine = [] {
            std::array<std::array<double, 8>, 8> table{};
            for (int u = 0; u < 8; ++u) {
                const double c = (u == 0) ? std::sqrt(0.5) : 1.0;
                for (int x = 0; x < 8; ++x) {
                    table[static_cast<std::size_t>(u)]
                         [static_cast<std::size_t>(x)] =
                        c * std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
                }
            }
            return table;
        }();
        std::array<std::array<double, 8>, 8> rows{};
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                double sum = 0.0;
                for (int u = 0; u < 8; ++u) {
                    sum += cosine[static_cast<std::size_t>(u)]
                                 [static_cast<std::size_t>(x)] *
                           block[static_cast<std::size_t>(y * 8 + u)];
                }
                rows[static_cast<std::size_t>(y)]
                    [static_cast<std::size_t>(x)] = sum;
            }
        }
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                double sum = 0.0;
                for (int v = 0; v < 8; ++v) {
                    sum += cosine[static_cast<std::size_t>(v)]
                                 [static_cast<std::size_t>(y)] *
                           rows[static_cast<std::size_t>(v)]
                               [static_cast<std::size_t>(x)];
                }
                block[static_cast<std::size_t>(y * 8 + x)] =
                    static_cast<int>(std::lround(0.25 * sum));
            }
        }
    }

    static int colour_component(double value) {
        return std::clamp(static_cast<int>(std::lround(value)), -128, 127) + 128;
    }

    void emit_colour(const Block& cr, const Block& cb,
                     const std::array<Block, 4>& luma) {
        std::array<std::array<int, 3>, 256> pixels{};
        for (int quadrant = 0; quadrant < 4; ++quadrant) {
            const int x0 = (quadrant & 1) * 8;
            const int y0 = (quadrant >> 1) * 8;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    const auto chroma = static_cast<std::size_t>(
                        ((x + x0) / 2) + ((y + y0) / 2) * 8);
                    const double red = cr[chroma];
                    const double blue = cb[chroma];
                    const double green = -0.3437 * blue - 0.7143 * red;
                    const int l = luma[static_cast<std::size_t>(quadrant)]
                                      [static_cast<std::size_t>(x + y * 8)];
                    const auto pixel =
                        static_cast<std::size_t>((x + x0) + (y + y0) * 16);
                    pixels[pixel] = {colour_component(l + 1.402 * red),
                                     colour_component(l + green),
                                     colour_component(l + 1.772 * blue)};
                }
            }
        }

        // The 15-bit bit controls the opaque flag of a 15-bit VRAM pixel;
        // unsigned output is the signed range shifted by 0x80, which is the
        // same as setting the top bit.
        const std::uint16_t high =
            (bit15_ || !signed_) ? 0x8000u : 0x0000u;
        if (depth_ == 3) {
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; x += 2) {
                    std::uint32_t word = 0;
                    for (int half = 0; half < 2; ++half) {
                        const auto& pixel =
                            pixels[static_cast<std::size_t>(x + half +
                                                            y * 16)];
                        const auto value = static_cast<std::uint16_t>(
                            ((pixel[0] >> 3) & 0x1F) |
                            (((pixel[1] >> 3) & 0x1F) << 5) |
                            (((pixel[2] >> 3) & 0x1F) << 10) | high);
                        word |= static_cast<std::uint32_t>(value) << (half * 16);
                    }
                    push_output(word);
                }
            }
            return;
        }

        // 24-bit output: one B, G, R triple per pixel, packed into words.
        std::vector<std::uint8_t> bytes;
        bytes.reserve(768);
        for (const auto& pixel : pixels) {
            // Signed output carries the value as a wrapped byte; unsigned
            // output is the same value shifted into 0..255.
            for (const auto component : pixel) {
                bytes.push_back(static_cast<std::uint8_t>(
                    signed_ ? (component - 128) : component));
            }
        }
        for (std::size_t index = 0; index < bytes.size(); index += 4) {
            const auto b0 = bytes[index];
            const auto b1 = index + 1 < bytes.size() ? bytes[index + 1] : 0;
            const auto b2 = index + 2 < bytes.size() ? bytes[index + 2] : 0;
            const auto b3 = index + 3 < bytes.size() ? bytes[index + 3] : 0;
            push_output(static_cast<std::uint32_t>(b0) |
                        (static_cast<std::uint32_t>(b1) << 8) |
                        (static_cast<std::uint32_t>(b2) << 16) |
                        (static_cast<std::uint32_t>(b3) << 24));
        }
    }

    void emit_mono(const Block& luma) {
        const int levels = depth_ == 0 ? 16 : 256;
        std::vector<std::uint8_t> bytes;
        bytes.reserve(64);
        for (const auto value : luma) {
            const int clipped =
                std::clamp(value & 0x1FF, -256, 255);
            const int centred = std::clamp(clipped, -128, 127) +
                                (signed_ ? 0 : 128);
            if (levels == 256) {
                bytes.push_back(
                    static_cast<std::uint8_t>(std::clamp(centred, 0, 255)));
            } else {
                bytes.push_back(static_cast<std::uint8_t>(
                    (std::clamp(centred, 0, 255) >> 4) & 0x0F));
            }
        }
        if (levels == 256) {
            for (std::size_t index = 0; index < bytes.size(); index += 4) {
                const auto b1 = index + 1 < bytes.size() ? bytes[index + 1] : 0;
                const auto b2 = index + 2 < bytes.size() ? bytes[index + 2] : 0;
                const auto b3 = index + 3 < bytes.size() ? bytes[index + 3] : 0;
                push_output(static_cast<std::uint32_t>(bytes[index]) |
                            (static_cast<std::uint32_t>(b1) << 8) |
                            (static_cast<std::uint32_t>(b2) << 16) |
                            (static_cast<std::uint32_t>(b3) << 24));
            }
        } else {
            for (std::size_t index = 0; index < bytes.size(); index += 8) {
                std::uint32_t word = 0;
                for (std::size_t half = 0; half < 8 && index + half < bytes.size();
                     ++half) {
                    word |= static_cast<std::uint32_t>(bytes[index + half])
                            << (half * 4);
                }
                push_output(word);
            }
        }
    }

    void push_output(std::uint32_t word) {
        output_.push_back(word);
        ++words_out_;
    }

    static constexpr std::array<int, 64> kZigZag = {
        0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
        3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
        10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
        21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

    std::array<std::uint8_t, 64> iq_y_{};
    std::array<std::uint8_t, 64> iq_uv_{};
    std::array<std::int16_t, 64> scale_{};

    std::vector<std::uint32_t> input_;
    std::vector<std::uint32_t> output_;
    std::size_t output_index_{0};

    std::size_t half_index_{0};
    std::size_t half_end_{0};

    int command_{0};
    std::size_t param_target_{0};
    bool colour_{false};
    std::uint32_t depth_{0};
    bool signed_{true};
    bool bit15_{false};
    bool enable_in_{false};
    bool enable_out_{false};

    std::uint64_t macroblocks_{0};
    std::uint64_t words_in_{0};
    std::uint64_t words_out_{0};
};

}  // namespace r6
