#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "r6/mdec_rl_table.h"

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
        bit_index_ = 0;
        stream_started_ = false;
        qscale_ = 1;
        colour_ = false;
        depth_ = 0;
        signed_ = true;
        bit15_ = false;
        enable_in_ = false;
        enable_out_ = false;
        macroblocks_ = 0;
        words_in_ = 0;
        words_out_ = 0;
        // The movie streams carry only the quantiser scale, never the
        // matrix, so the tables start at the MPEG-1 intra matrix the
        // encoder used.
        iq_y_ = kDefaultIntraQuant;
        iq_uv_ = kDefaultIntraQuant;
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
                bit_index_ = 0;
                stream_started_ = false;
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
        if (command_ == 1 && input_.size() <= 9 &&
            std::getenv("MDEC_TRACE") != nullptr) {
            std::fprintf(stderr, "mdec word[%zu] = %08X (size %zu)\n",
                         input_.size() - 1, value, param_target_);
        }
        if (command_ == 1) {
            decode_available();
            if (input_.size() >= param_target_) {
                command_ = 0;
                input_.clear();
                bit_index_ = 0;
                stream_started_ = false;
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
    std::size_t bits_used() const { return bit_index_; }
    bool stream_started() const { return stream_started_; }
    std::uint64_t failed_blocks() const { return failed_blocks_; }
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

    // Decodes as far as the buffered words allow and leaves the read position
    // at the last macroblock boundary, so a macroblock whose bits are still in
    // flight is retried when the player's next transfer arrives.
    //
    // The stream is the STR payload byte for byte, and it is bit packed rather
    // than a sequence of 16-bit codes: a 4-byte preamble, the quantiser scale,
    // the stream version, then per macroblock six blocks in the order Cr, Cb,
    // Y0..Y3, each starting with a signed 10-bit DC that is used doubled.
    void decode_available() {
        if (!stream_started_) {
            if (available_bits() < 64) return;
            qscale_ = static_cast<int>(bits_at(32, 16));
            if (qscale_ <= 0) qscale_ = 1;
            version_ = static_cast<int>(bits_at(48, 16));
            bit_index_ = 64;
            stream_started_ = true;
        }
        for (;;) {
            const auto start = bit_index_;
            bool complete = true;
            if (depth_ <= 1) {
                Block luma{};
                complete = decode_block(iq_y_, luma);
                if (complete) emit_mono(luma);
            } else {
                Block cr{};
                Block cb{};
                std::array<Block, 4> luma{};
                complete =
                    decode_block(iq_uv_, cr) && decode_block(iq_uv_, cb);
                for (int quadrant = 0; complete && quadrant < 4; ++quadrant) {
                    complete = decode_block(
                        iq_y_, luma[static_cast<std::size_t>(quadrant)]);
                }
                if (complete) emit_colour(cr, cb, luma);
            }
            if (!complete) {
                bit_index_ = start;
                ++failed_blocks_;
                break;
            }
            ++macroblocks_;
            if (bit_index_ >= available_bits()) break;
        }
    }

    std::size_t available_bits() const { return input_.size() * 32; }

    // The words arrive in memory order, and each 16-bit unit of the stream is
    // stored byte-swapped, so the stream byte at `index` is the data byte at
    // index ^ 1. Bits run most significant first from there.
    std::uint8_t stream_byte(std::size_t index) const {
        const auto data = index ^ 1u;
        const auto word = input_[data / 4];
        return static_cast<std::uint8_t>((word >> ((data % 4) * 8)) & 0xFFu);
    }

    std::uint32_t bits_at(std::size_t position, int count) const {
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            const auto at = position + static_cast<std::size_t>(index);
            const auto byte = stream_byte(at / 8);
            value = (value << 1) | ((byte >> (7 - (at % 8))) & 1u);
        }
        return value;
    }

    static int signed_value(std::uint32_t value, int count) {
        const std::uint32_t sign = 1u << (count - 1);
        return (value & sign) != 0 ? static_cast<int>(value) - (1 << count)
                                   : static_cast<int>(value);
    }

    // One block: the 10-bit DC, then MPEG-1 run/level codes with a sign bit
    // each. The scan starts at the first AC coefficient, `10` ends the block
    // and `000001` is the escape that carries the run and level as literals.
    // Returns false when the buffered words run out, so the caller can retry.
    bool decode_block(const std::array<std::uint8_t, 64>& quant, Block& block) {
        block.fill(0);
        if (bit_index_ + 10 > available_bits()) return false;
        block[0] = 2 * signed_value(bits_at(bit_index_, 10), 10);
        bit_index_ += 10;
        int scan = 1;
        for (;;) {
            const RlCode* found = nullptr;
            int length = 0;
            for (int candidate = 1; candidate <= 16 && found == nullptr;
                 ++candidate) {
                if (bit_index_ + static_cast<std::size_t>(candidate) >
                    available_bits()) {
                    return false;
                }
                const auto code = bits_at(bit_index_, candidate);
                for (const auto& entry : kRlCodes) {
                    if (entry.bits == candidate && entry.code == code) {
                        found = &entry;
                        length = candidate;
                        break;
                    }
                }
            }
            if (found == nullptr) return false;
            bit_index_ += static_cast<std::size_t>(length);
            if (found->kind == 2) return true;  // end of block
            if (found->kind == 1) {             // escape: literal run and level
                if (bit_index_ + 16 > available_bits()) return false;
                scan += static_cast<int>(bits_at(bit_index_, 6)) + 1;
                const int level =
                    signed_value(bits_at(bit_index_ + 6, 10), 10);
                bit_index_ += 16;
                if (scan > 63) return true;  // malformed, nothing left to place
                const auto raster =
                    kScanToRaster[static_cast<std::size_t>(scan)];
                block[raster] = (level * qscale_ * quant[raster]) >> 3;
                continue;
            }
            scan += found->run;
            if (bit_index_ + 1 > available_bits()) return false;
            const int sign = bits_at(bit_index_, 1) != 0 ? -1 : 1;
            ++bit_index_;
            if (scan > 63) return true;
            const auto raster = kScanToRaster[static_cast<std::size_t>(scan)];
            block[raster] =
                sign * ((found->level * qscale_ * quant[raster]) >> 3);
        }
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

    // The movie stream's scan order, which is the MPEG-1 one; the run
    // length table above is MPEG-1's, and this is the order its runs index.
    static constexpr std::array<std::uint8_t, 64> kScanToRaster = {
        0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

    static constexpr std::array<std::uint8_t, 64> kDefaultIntraQuant = {
        8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
        19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
        22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
        26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

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

    std::size_t bit_index_{0};
    bool stream_started_{false};
    int qscale_{1};
    int version_{2};

    int command_{0};
    std::size_t param_target_{0};
    bool colour_{false};
    std::uint32_t depth_{0};
    bool signed_{true};
    bool bit15_{false};
    bool enable_in_{false};
    bool enable_out_{false};

    std::uint64_t macroblocks_{0};
    std::uint64_t failed_blocks_{0};
    std::uint64_t words_in_{0};
    std::uint64_t words_out_{0};
};

}  // namespace r6
