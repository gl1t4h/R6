#include "r6/chd_image.h"

#include <algorithm>
#include <array>
#include <cstring>

// The layout implemented here follows MAME's `chd.cpp` version 5 structures.
// All integer fields in the container are big-endian, including the bit
// stream that holds the compressed hunk map.  Hunk payloads are raw DEFLATE
// or headerless LZMA streams, so both decoders below work on known-size
// output buffers and never need to parse a wrapper header.
//
// R6 only ever asks for CD sectors, so the container is required to use the
// CD frame size (2448 bytes: 2352 sector bytes plus 96 subcode bytes).  That
// keeps the address arithmetic here identical to the raw BIN reader: the
// guest's LBA is the frame index.

namespace r6 {
namespace {

constexpr std::uint32_t kFrameBytes = 2448;   // 2352 raw + 96 subcode
constexpr std::uint32_t kSectorBytes = 2352;
constexpr std::uint32_t kSubcodeBytes = 96;
constexpr std::uint32_t kUserOffset = 24;
constexpr std::uint32_t kUserBytes = 2048;
constexpr std::uint32_t kHeaderOffset = 12;
constexpr std::uint32_t kHeaderBytes = 12;

constexpr std::uint32_t kCodecZlib = 0x7A6C6962;  // 'zlib'
constexpr std::uint32_t kCodecLzma = 0x6C7A6D61;  // 'lzma'
constexpr std::uint32_t kCodecHuffman = 0x68756666;  // 'huff'
constexpr std::uint32_t kCodecFlac = 0x666C6163;  // 'flac'
constexpr std::uint32_t kCodecCdZlib = 0x63647A6C;  // 'cdzl'
constexpr std::uint32_t kCodecCdLzma = 0x63646C7A;  // 'cdlz'
constexpr std::uint32_t kCodecCdFlac = 0x6364666C;  // 'cdfl'
constexpr std::uint32_t kCodecZstd = 0x7A737464;  // 'zstd'
constexpr std::uint32_t kCodecCdZstd = 0x63647A73;  // 'cdzs'
constexpr std::uint32_t kCodecAvhuff = 0x61766875;  // 'avhu'

// Map entry types, from chd.cpp's COMPRESSION_* enum.
constexpr std::uint8_t kTypeCodec0 = 0;
constexpr std::uint8_t kTypeNone = 4;
constexpr std::uint8_t kTypeSelf = 5;
constexpr std::uint8_t kTypeParent = 6;
constexpr std::uint8_t kTypeRleSmall = 7;
constexpr std::uint8_t kTypeRleLarge = 8;
constexpr std::uint8_t kTypeSelf0 = 9;
constexpr std::uint8_t kTypeSelf1 = 10;
constexpr std::uint8_t kTypeParentSelf = 11;
constexpr std::uint8_t kTypeParent0 = 12;
constexpr std::uint8_t kTypeParent1 = 13;

std::uint32_t be32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t be48(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int index = 0; index < 6; ++index) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

std::uint64_t be64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

std::string codec_name(std::uint32_t codec) {
    std::string name(4, ' ');
    for (int index = 0; index < 4; ++index) {
        name[static_cast<std::size_t>(index)] = static_cast<char>(
            (codec >> (24 - index * 8)) & 0xFFu);
    }
    return name;
}

// ---------------------------------------------------------------------------
// Big-endian bit stream (the map encoding reads bits from the most
// significant end, matching MAME's bitstream_in).
// ---------------------------------------------------------------------------

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    std::uint32_t read(int count) {
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            value <<= 1;
            const auto byte_index = bit_ / 8;
            if (byte_index < size_) {
                const auto byte = data_[byte_index];
                value |= (byte >> (7 - (bit_ % 8))) & 1u;
            }
            ++bit_;
        }
        return value;
    }

    bool overflowed() const { return bit_ / 8 > size_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bit_{0};
};

// Static Huffman table used by the compressed hunk map.  The encoding in
// MAME is `huffman_decoder<16, 8>`: 16 symbols, at most 8 bits each, tree
// lengths written with 4-bit fields and run-length escapes.
class MapHuffman {
public:
    bool import(BitReader& bits) {
        std::uint8_t lengths[16]{};
        int node = 0;
        while (node < 16) {
            const auto value = bits.read(4);
            if (value != 1) {
                lengths[node++] = static_cast<std::uint8_t>(value);
                continue;
            }
            const auto repeated = bits.read(4);
            if (repeated == 1) {
                lengths[node++] = 1;
                continue;
            }
            const auto count = bits.read(4) + 3;
            if (node + static_cast<int>(count) > 16) return false;
            for (std::uint32_t repeat = 0; repeat < count; ++repeat) {
                lengths[node++] = static_cast<std::uint8_t>(repeated);
            }
        }
        return assign(lengths);
    }

    std::uint8_t decode(BitReader& bits) const {
        std::uint32_t code = 0;
        for (int length = 1; length <= 8; ++length) {
            code = (code << 1) | bits.read(1);
            for (int symbol = 0; symbol < 16; ++symbol) {
                if (lengths_[symbol] == length && codes_[symbol] == code) {
                    return static_cast<std::uint8_t>(symbol);
                }
            }
        }
        return 0xFF;
    }

private:
    // Mirrors huffman_context_base::assign_canonical_codes(), which starts
    // from the longest code and walks down to one bit.
    bool assign(const std::uint8_t lengths[16]) {
        std::uint32_t histogram[33]{};
        for (int symbol = 0; symbol < 16; ++symbol) {
            if (lengths[symbol] > 8) return false;
            if (lengths[symbol] > 0) ++histogram[lengths[symbol]];
        }
        std::uint32_t start = 0;
        for (int length = 32; length > 0; --length) {
            const auto next = (start + histogram[length]) >> 1;
            if (length != 1 && next * 2 != start + histogram[length]) {
                return false;
            }
            histogram[length] = start;
            start = next;
        }
        for (int symbol = 0; symbol < 16; ++symbol) {
            lengths_[symbol] = lengths[symbol];
            codes_[symbol] =
                lengths[symbol] == 0 ? 0 : histogram[lengths[symbol]]++;
        }
        return true;
    }

    std::uint8_t lengths_[16]{};
    std::uint32_t codes_[16]{};
};

// ---------------------------------------------------------------------------
// Raw DEFLATE (RFC 1951), the payload format of the zlib/cdzl hunks.
// ---------------------------------------------------------------------------

class BitInput {
public:
    BitInput(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    std::uint32_t bits(int count) {
        while (bit_count_ < count) {
            const std::uint32_t byte = position_ < size_ ? data_[position_] : 0;
            ++position_;
            buffer_ |= byte << bit_count_;
            bit_count_ += 8;
        }
        const std::uint32_t value = buffer_ & ((1u << count) - 1u);
        buffer_ >>= count;
        bit_count_ -= count;
        return value;
    }

    void align() {
        buffer_ = 0;
        bit_count_ = 0;
    }

    std::size_t position() const { return position_; }
    std::size_t size() const { return size_; }
    const std::uint8_t* data() const { return data_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_{0};
    std::uint32_t buffer_{0};
    int bit_count_{0};
};

struct HuffmanTable {
    std::uint16_t counts[16]{};
    std::uint16_t symbols[288]{};
};

bool build_huffman(const std::uint8_t* lengths, int count,
                   HuffmanTable& table) {
    for (int index = 0; index < count; ++index) {
        ++table.counts[lengths[index]];
    }
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= table.counts[length];
        if (left < 0) return false;
    }
    std::uint16_t offsets[16]{};
    for (int length = 1; length < 15; ++length) {
        offsets[length + 1] =
            static_cast<std::uint16_t>(offsets[length] + table.counts[length]);
    }
    for (int symbol = 0; symbol < count; ++symbol) {
        if (lengths[symbol] != 0) {
            table.symbols[offsets[lengths[symbol]]++] =
                static_cast<std::uint16_t>(symbol);
        }
    }
    return true;
}

bool decode_symbol(BitInput& input, const HuffmanTable& table, int& symbol) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= 15; ++length) {
        code |= static_cast<int>(input.bits(1));
        const int count = table.counts[length];
        if (code - first < count) {
            symbol = table.symbols[index + (code - first)];
            return true;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return false;
}

const std::uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};
const std::uint8_t kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_raw(const std::uint8_t* source, std::size_t source_size,
                 std::uint8_t* destination, std::size_t destination_size) {
    BitInput input(source, source_size);
    std::size_t out = 0;

    bool final_block = false;
    while (!final_block) {
        final_block = input.bits(1) != 0;
        const auto type = input.bits(2);
        if (type == 0) {
            input.align();
            const std::uint32_t length = input.bits(16);
            const std::uint32_t complement = input.bits(16);
            if ((length ^ 0xFFFFu) != complement) return false;
            for (std::uint32_t index = 0; index < length; ++index) {
                // Stored blocks are byte aligned, so pull them straight out.
                const auto position = input.position() + index;
                if (position >= input.size()) return false;
                if (out < destination_size) {
                    destination[out++] = input.data()[position];
                }
            }
            // bits() drained the accumulator; skip the copied bytes.
            for (std::uint32_t index = 0; index < length; ++index) {
                input.bits(8);
            }
            continue;
        }
        if (type == 3) return false;

        HuffmanTable literals;
        HuffmanTable distances;
        if (type == 1) {
            std::uint8_t literal_lengths[288]{};
            for (int symbol = 0; symbol < 144; ++symbol) {
                literal_lengths[symbol] = 8;
            }
            for (int symbol = 144; symbol < 256; ++symbol) {
                literal_lengths[symbol] = 9;
            }
            for (int symbol = 256; symbol < 280; ++symbol) {
                literal_lengths[symbol] = 7;
            }
            for (int symbol = 280; symbol < 288; ++symbol) {
                literal_lengths[symbol] = 8;
            }
            std::uint8_t distance_lengths[32]{};
            for (auto& length : distance_lengths) length = 5;
            if (!build_huffman(literal_lengths, 288, literals)) return false;
            if (!build_huffman(distance_lengths, 32, distances)) return false;
        } else {
            const auto literal_count = static_cast<int>(input.bits(5)) + 257;
            const auto distance_count = static_cast<int>(input.bits(5)) + 1;
            const auto code_count = static_cast<int>(input.bits(4)) + 4;
            static const std::uint8_t kOrder[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                12, 3, 13, 2, 14, 1, 15};
            std::uint8_t code_lengths[19]{};
            for (int index = 0; index < code_count; ++index) {
                code_lengths[kOrder[index]] =
                    static_cast<std::uint8_t>(input.bits(3));
            }
            HuffmanTable codes;
            if (!build_huffman(code_lengths, 19, codes)) return false;

            std::uint8_t lengths[320]{};
            int index = 0;
            while (index < literal_count + distance_count) {
                int symbol = 0;
                if (!decode_symbol(input, codes, symbol)) return false;
                if (symbol < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(symbol);
                } else if (symbol == 16) {
                    if (index == 0) return false;
                    const auto repeat = static_cast<int>(input.bits(2)) + 3;
                    const auto value = lengths[index - 1];
                    for (int count = 0; count < repeat; ++count) {
                        lengths[index++] = value;
                    }
                } else if (symbol == 17) {
                    const auto repeat = static_cast<int>(input.bits(3)) + 3;
                    index += repeat;
                } else if (symbol == 18) {
                    const auto repeat = static_cast<int>(input.bits(7)) + 11;
                    index += repeat;
                } else {
                    return false;
                }
                if (index > literal_count + distance_count) return false;
            }
            if (!build_huffman(lengths, literal_count, literals)) return false;
            if (!build_huffman(lengths + literal_count, distance_count,
                               distances)) {
                return false;
            }
        }

        for (;;) {
            int symbol = 0;
            if (!decode_symbol(input, literals, symbol)) return false;
            if (symbol < 256) {
                if (out >= destination_size) return false;
                destination[out++] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            if (symbol == 256) break;

            const int length_index = symbol - 257;
            if (length_index < 0 || length_index >= 29) return false;
            std::uint32_t length = kLengthBase[length_index];
            if (kLengthExtra[length_index] != 0) {
                length += input.bits(kLengthExtra[length_index]);
            }
            int distance_symbol = 0;
            if (!decode_symbol(input, distances, distance_symbol)) return false;
            if (distance_symbol < 0 || distance_symbol >= 30) return false;
            std::uint32_t distance = kDistanceBase[distance_symbol];
            if (kDistanceExtra[distance_symbol] != 0) {
                distance += input.bits(kDistanceExtra[distance_symbol]);
            }
            if (distance == 0 || distance > out) return false;
            if (length > destination_size - out) return false;
            for (std::uint32_t count = 0; count < length; ++count) {
                destination[out] = destination[out - distance];
                ++out;
            }
        }
    }
    return out == destination_size;
}

// ---------------------------------------------------------------------------
// Headerless LZMA1, the payload format of the lzma/cdlz hunks.
//
// MAME encodes every hunk independently with lc=3, lp=0, pb=2 and a
// dictionary no larger than the hunk, so a decoder with a full-output
// window and a hard output length is enough.
// ---------------------------------------------------------------------------

class RangeDecoder {
public:
    RangeDecoder(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool start() {
        range_ = 0xFFFFFFFFu;
        code_ = 0;
        for (int index = 0; index < 5; ++index) {
            if (position_ >= size_) return false;
            code_ = (code_ << 8) | data_[position_++];
        }
        return true;
    }

    std::uint32_t bit(std::uint16_t& probability) {
        const std::uint32_t bound = (range_ >> 11) * probability;
        std::uint32_t result = 0;
        if (code_ < bound) {
            range_ = bound;
            probability = static_cast<std::uint16_t>(
                probability + ((2048 - probability) >> 5));
        } else {
            range_ -= bound;
            code_ -= bound;
            probability =
                static_cast<std::uint16_t>(probability - (probability >> 5));
            result = 1;
        }
        if (range_ < (1u << 24)) {
            range_ <<= 8;
            code_ = (code_ << 8) | next();
        }
        return result;
    }

    std::uint32_t direct(int count) {
        std::uint32_t result = 0;
        for (int index = 0; index < count; ++index) {
            range_ >>= 1;
            code_ -= range_;
            const std::uint32_t borrow = 0u - (code_ >> 31);
            code_ += range_ & borrow;
            result = (result << 1) + (borrow + 1u);
            if (range_ < (1u << 24)) {
                range_ <<= 8;
                code_ = (code_ << 8) | next();
            }
        }
        return result;
    }

    bool exhausted() const { return position_ > size_; }

private:
    std::uint8_t next() { return position_ < size_ ? data_[position_++] : 0; }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_{0};
    std::uint32_t range_{0xFFFFFFFFu};
    std::uint32_t code_{0};
};

std::uint32_t decode_tree(RangeDecoder& range, std::uint16_t* probabilities,
                          int bits) {
    std::uint32_t symbol = 1;
    for (int index = 0; index < bits; ++index) {
        symbol = (symbol << 1) | range.bit(probabilities[symbol]);
    }
    return symbol - (1u << bits);
}

std::uint32_t decode_reverse_tree(RangeDecoder& range,
                                  std::uint16_t* probabilities, int bits) {
    std::uint32_t symbol = 0;
    std::uint32_t index = 1;
    for (int bit_index = 0; bit_index < bits; ++bit_index) {
        const std::uint32_t value = range.bit(probabilities[index]);
        index = (index << 1) | value;
        symbol |= value << bit_index;
    }
    return symbol;
}

struct LengthDecoder {
    std::uint16_t choice{1024};
    std::uint16_t choice2{1024};
    std::uint16_t low[16 * 8]{};
    std::uint16_t mid[16 * 8]{};
    std::uint16_t high[256]{};

    void reset() {
        choice = choice2 = 1024;
        std::fill(std::begin(low), std::end(low), 1024);
        std::fill(std::begin(mid), std::end(mid), 1024);
        std::fill(std::begin(high), std::end(high), 1024);
    }

    std::uint32_t decode(RangeDecoder& range, int pos_state) {
        if (range.bit(choice) == 0) {
            return decode_tree(range, low + pos_state * 8, 3);
        }
        if (range.bit(choice2) == 0) {
            return 8 + decode_tree(range, mid + pos_state * 8, 3);
        }
        return 16 + decode_tree(range, high, 8);
    }
};

class LzmaDecoder {
public:
    LzmaDecoder() { reset(); }

    bool decode(const std::uint8_t* source, std::size_t source_size,
                std::uint8_t* destination, std::size_t destination_size) {
        RangeDecoder range(source, source_size);
        if (!range.start()) return false;
        reset();

        std::uint32_t rep0 = 0;
        std::uint32_t rep1 = 0;
        std::uint32_t rep2 = 0;
        std::uint32_t rep3 = 0;
        std::uint32_t state = 0;
        std::size_t position = 0;

        while (position < destination_size) {
            const auto pos_state = static_cast<int>(position) & 3;
            if (range.bit(is_match_[state * 16 + pos_state]) == 0) {
                const auto previous =
                    position > 0 ? destination[position - 1] : 0;
                auto* literal = literals_ + 0x300 * (previous >> 5);
                std::uint32_t symbol = 1;
                if (state < 7) {
                    while (symbol < 0x100) {
                        symbol = (symbol << 1) | range.bit(literal[symbol]);
                    }
                } else {
                    if (rep0 >= position) return false;
                    std::uint32_t match_byte = destination[position - rep0 - 1];
                    while (symbol < 0x100) {
                        const std::uint32_t match_bit = (match_byte >> 7) & 1;
                        match_byte <<= 1;
                        const std::uint32_t value = range.bit(
                            literal[((1u + match_bit) << 8) + symbol]);
                        symbol = (symbol << 1) | value;
                        if (match_bit != value) {
                            while (symbol < 0x100) {
                                symbol =
                                    (symbol << 1) | range.bit(literal[symbol]);
                            }
                            break;
                        }
                    }
                }
                destination[position++] = static_cast<std::uint8_t>(symbol);
                state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
                continue;
            }

            std::uint32_t length = 0;
            if (range.bit(is_rep_[state]) != 0) {
                if (range.bit(is_rep_g0_[state]) == 0) {
                    if (range.bit(is_rep0_long_[state * 16 + pos_state]) == 0) {
                        if (rep0 >= position) return false;
                        destination[position] =
                            destination[position - rep0 - 1];
                        ++position;
                        state = state < 7 ? 9 : 11;
                        continue;
                    }
                } else {
                    // Only a match taken from rep2/rep3 rotates rep2; taking
                    // rep1 leaves rep2 alone.  This is the LZMA reference
                    // state machine and getting it wrong corrupts later
                    // distances while the first few bytes still look right.
                    std::uint32_t distance = 0;
                    if (range.bit(is_rep_g1_[state]) == 0) {
                        distance = rep1;
                    } else {
                        if (range.bit(is_rep_g2_[state]) == 0) {
                            distance = rep2;
                        } else {
                            distance = rep3;
                            rep3 = rep2;
                        }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = distance;
                }
                length = rep_length_.decode(range, pos_state) + 2;
                state = state < 7 ? 8 : 11;
            } else {
                rep3 = rep2;
                rep2 = rep1;
                rep1 = rep0;
                length = length_.decode(range, pos_state) + 2;
                state = state < 7 ? 7 : 10;
                rep0 = decode_distance(range, length);
                if (rep0 == 0xFFFFFFFFu) break;  // end marker, not expected
            }
            if (rep0 >= position) return false;
            if (length > destination_size - position) return false;
            for (std::uint32_t index = 0; index < length; ++index) {
                destination[position] = destination[position - rep0 - 1];
                ++position;
            }
        }
        return position == destination_size && !range.exhausted();
    }

private:
    void reset() {
        std::fill(std::begin(is_match_), std::end(is_match_), 1024);
        std::fill(std::begin(is_rep_), std::end(is_rep_), 1024);
        std::fill(std::begin(is_rep_g0_), std::end(is_rep_g0_), 1024);
        std::fill(std::begin(is_rep_g1_), std::end(is_rep_g1_), 1024);
        std::fill(std::begin(is_rep_g2_), std::end(is_rep_g2_), 1024);
        std::fill(std::begin(is_rep0_long_), std::end(is_rep0_long_), 1024);
        std::fill(std::begin(pos_slot_), std::end(pos_slot_), 1024);
        std::fill(std::begin(pos_decoders_), std::end(pos_decoders_), 1024);
        std::fill(std::begin(align_), std::end(align_), 1024);
        std::fill(std::begin(literals_), std::end(literals_), 1024);
        length_.reset();
        rep_length_.reset();
    }

    std::uint32_t decode_distance(RangeDecoder& range, std::uint32_t length) {
        const auto length_state = length - 2 < 4 ? length - 2 : 3;
        const auto pos_slot =
            decode_tree(range, pos_slot_ + length_state * 64, 6);
        if (pos_slot < 4) return pos_slot;
        const auto direct_bits = static_cast<int>((pos_slot >> 1) - 1);
        std::uint32_t distance = (2u | (pos_slot & 1u)) << direct_bits;
        if (pos_slot < 14) {
            distance += decode_reverse_tree(
                range, pos_decoders_ + (distance - pos_slot - 1), direct_bits);
        } else {
            distance += range.direct(direct_bits - 4) << 4;
            distance += decode_reverse_tree(range, align_, 4);
        }
        return distance;
    }

    std::uint16_t is_match_[12 * 16]{};
    std::uint16_t is_rep_[12]{};
    std::uint16_t is_rep_g0_[12]{};
    std::uint16_t is_rep_g1_[12]{};
    std::uint16_t is_rep_g2_[12]{};
    std::uint16_t is_rep0_long_[12 * 16]{};
    std::uint16_t pos_slot_[4 * 64]{};
    std::uint16_t pos_decoders_[114]{};
    std::uint16_t align_[16]{};
    std::uint16_t literals_[0x300 * 8]{};
    LengthDecoder length_;
    LengthDecoder rep_length_;
};

bool decompress_payload(std::uint32_t codec, const std::uint8_t* source,
                        std::size_t source_size, std::uint8_t* destination,
                        std::size_t destination_size, std::string& error) {
    if (codec == kCodecZlib) {
        if (!inflate_raw(source, source_size, destination, destination_size)) {
            error = "DEFLATE hunk did not decode";
            return false;
        }
        return true;
    }
    if (codec == kCodecLzma) {
        LzmaDecoder decoder;
        if (!decoder.decode(source, source_size, destination,
                            destination_size)) {
            error = "LZMA hunk did not decode";
            return false;
        }
        return true;
    }
    if (codec == kCodecFlac) {
        error = "hunk is FLAC-compressed, which R6 does not decode";
        return false;
    }
    error = "unsupported CHD hunk codec '" + codec_name(codec) + "'";
    return false;
}

// CD hunks are stored as the sector payload and subcode streams, each
// compressed separately, behind a small per-hunk header.  Only the sector
// stream is needed for reads, but both must be decompressed to know the
// offsets.
bool decode_cd_hunk(std::uint32_t codec, const std::uint8_t* source,
                    std::size_t source_size, std::uint8_t* destination,
                    std::size_t destination_size, std::string& error) {
    if (destination_size % kFrameBytes != 0) {
        error = "CD hunk is not a whole number of frames";
        return false;
    }
    const auto frames = destination_size / kFrameBytes;
    const std::size_t length_bytes = destination_size < 65536 ? 2 : 3;
    const std::size_t ecc_bytes = (frames + 7) / 8;
    const std::size_t header_bytes = ecc_bytes + length_bytes;
    if (source_size < header_bytes) {
        error = "CD hunk header is truncated";
        return false;
    }

    std::size_t base_length = 0;
    for (std::size_t index = 0; index < length_bytes; ++index) {
        base_length = (base_length << 8) | source[ecc_bytes + index];
    }
    if (base_length > source_size - header_bytes) {
        error = "CD hunk payload is truncated";
        return false;
    }

    // The CD codec tag names the wrapper; the base stream inside uses the
    // plain codec (cdzl is DEFLATE, cdlz is LZMA).
    const auto base_codec =
        codec == kCodecCdZlib ? kCodecZlib : kCodecLzma;
    std::vector<std::uint8_t> sectors(frames * kSectorBytes);
    if (!decompress_payload(base_codec, source + header_bytes, base_length,
                            sectors.data(), sectors.size(), error)) {
        return false;
    }

    std::vector<std::uint8_t> subcode(frames * kSubcodeBytes);
    const auto* subcode_source = source + header_bytes + base_length;
    const auto subcode_size = source_size - header_bytes - base_length;
    if (!inflate_raw(subcode_source, subcode_size, subcode.data(),
                     subcode.size())) {
        error = "CD subcode stream did not decode";
        return false;
    }

    static const std::uint8_t kSync[12] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        auto* target = destination + frame * kFrameBytes;
        std::memcpy(target, sectors.data() + frame * kSectorBytes,
                    kSectorBytes);
        std::memcpy(target + kSectorBytes,
                    subcode.data() + frame * kSubcodeBytes, kSubcodeBytes);
        // The compressor blanks the sync pattern when the sector had valid
        // ECC and stores one bit per frame saying so.  The EDC/ECC bytes are
        // left zeroed; the CD controller only exposes bytes 12..2072.
        if ((source[frame / 8] & (1u << (frame % 8))) != 0) {
            std::memcpy(target, kSync, sizeof kSync);
        }
    }
    return true;
}

}  // namespace

bool ChdImage::open(const std::filesystem::path& path, std::string& error) {
    file_.open(path, std::ios::binary);
    if (!file_) {
        error = "cannot open " + path.string();
        return false;
    }
    file_.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::uint64_t>(file_.tellg());
    if (file_size < 124) {
        error = "file is too small to be a CHD";
        return false;
    }

    std::uint8_t header[124]{};
    if (!read_at(0, header, sizeof header)) {
        error = "cannot read the CHD header";
        return false;
    }
    if (std::memcmp(header, "MComprHD", 8) != 0) {
        error = "not a CHD file (missing MComprHD tag)";
        return false;
    }
    const auto version = be32(header + 12);
    if (version != 5) {
        error = "CHD version " + std::to_string(version) +
                " is not supported (version 5 only)";
        return false;
    }

    bool has_parent = false;
    for (int index = 104; index < 124; ++index) {
        if (header[index] != 0) has_parent = true;
    }
    if (has_parent) {
        error = "delta CHDs with a parent image are not supported";
        return false;
    }

    for (int index = 0; index < 4; ++index) {
        compressors_[index] = be32(header + 16 + index * 4);
    }
    logical_bytes_ = be64(header + 32);
    map_offset_ = be64(header + 40);
    hunk_bytes_ = be32(header + 56);
    frame_bytes_ = be32(header + 60);

    if (frame_bytes_ != kFrameBytes) {
        error = "not a CD image (frame size " +
                std::to_string(frame_bytes_) + ", expected " +
                std::to_string(kFrameBytes) + ")";
        return false;
    }
    if (hunk_bytes_ == 0 || hunk_bytes_ > 512 * 1024 ||
        hunk_bytes_ % kFrameBytes != 0) {
        error = "unsupported hunk size " + std::to_string(hunk_bytes_);
        return false;
    }
    if (logical_bytes_ == 0) {
        error = "CHD has no logical data";
        return false;
    }
    sector_count_ = static_cast<std::uint32_t>(logical_bytes_ / kFrameBytes);
    hunk_count_ = static_cast<std::uint32_t>(
        (logical_bytes_ + hunk_bytes_ - 1) / hunk_bytes_);
    if (hunk_count_ == 0) {
        error = "CHD has no hunks";
        return false;
    }

    error_.clear();
    if (!load_map(error)) {
        file_.close();
        return false;
    }
    buffer_.assign(hunk_bytes_, 0);
    cached_hunk_ = 0xFFFFFFFFu;
    open_ = true;
    return true;
}

bool ChdImage::read_at(std::uint64_t offset, std::uint8_t* destination,
                       std::size_t size) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(destination),
               static_cast<std::streamsize>(size));
    return file_.gcount() == static_cast<std::streamsize>(size);
}

std::uint8_t ChdImage::codec_at(std::uint8_t type) const {
    return type < 4 ? type : 0xFF;
}

bool ChdImage::load_map(std::string& error) {
    map_.assign(hunk_count_, MapEntry{});
    if (compressors_[0] == 0) {
        // Uncompressed CHDs store one 32-bit value per hunk: the offset in
        // units of hunk size.  Zero means a zero-filled hunk.
        std::vector<std::uint8_t> raw(
            static_cast<std::size_t>(hunk_count_) * 4);
        if (!read_at(map_offset_, raw.data(), raw.size())) {
            error = "cannot read the hunk map";
            return false;
        }
        for (std::uint32_t index = 0; index < hunk_count_; ++index) {
            const auto value =
                be32(raw.data() + static_cast<std::size_t>(index) * 4);
            auto& entry = map_[index];
            entry.type = kTypeNone;
            entry.length = hunk_bytes_;
            entry.offset = static_cast<std::uint64_t>(value) * hunk_bytes_;
        }
        return true;
    }
    return load_compressed_map(error);
}

bool ChdImage::load_compressed_map(std::string& error) {
    std::uint8_t header[16]{};
    if (!read_at(map_offset_, header, sizeof header)) {
        error = "cannot read the compressed hunk map header";
        return false;
    }
    const auto payload_size = be32(header);
    const auto data_start = be48(header + 4);
    const auto length_bits = header[12];
    const auto self_bits = header[13];
    const auto parent_bits = header[14];
    if (payload_size == 0 || payload_size > 64 * 1024 * 1024) {
        error = "compressed hunk map has an invalid size";
        return false;
    }

    std::vector<std::uint8_t> payload(payload_size);
    if (!read_at(map_offset_ + 16, payload.data(), payload.size())) {
        error = "cannot read the compressed hunk map";
        return false;
    }

    BitReader bits(payload.data(), payload.size());
    MapHuffman huffman;
    if (!huffman.import(bits)) {
        error = "compressed hunk map has an invalid Huffman tree";
        return false;
    }

    // First pass: the RLE-coded compression type of every hunk.
    std::uint8_t last = 0;
    int repeat = 0;
    for (std::uint32_t index = 0; index < hunk_count_; ++index) {
        auto& entry = map_[index];
        if (repeat > 0) {
            entry.type = last;
            --repeat;
            continue;
        }
        const auto value = huffman.decode(bits);
        if (value == 0xFF) {
            error = "compressed hunk map uses an invalid Huffman code";
            return false;
        }
        if (value == kTypeRleSmall) {
            entry.type = last;
            repeat = 2 + static_cast<int>(huffman.decode(bits));
        } else if (value == kTypeRleLarge) {
            entry.type = last;
            repeat = 2 + 16 + (static_cast<int>(huffman.decode(bits)) << 4) +
                     static_cast<int>(huffman.decode(bits));
        } else {
            last = value;
            entry.type = value;
        }
    }

    // Second pass: the per-hunk fields that follow the compression types.
    std::uint64_t offset = data_start;
    std::uint32_t last_self = 0;
    std::uint64_t last_parent = 0;
    const auto units_per_hunk = hunk_bytes_ / frame_bytes_;
    for (std::uint32_t index = 0; index < hunk_count_; ++index) {
        auto& entry = map_[index];
        auto block_offset = offset;
        std::uint32_t length = 0;
        switch (entry.type) {
            case kTypeCodec0:
            case kTypeCodec0 + 1:
            case kTypeCodec0 + 2:
            case kTypeCodec0 + 3:
                length = bits.read(length_bits);
                bits.read(16);  // CRC-16 of the uncompressed hunk
                offset += length;
                break;
            case kTypeNone:
                length = hunk_bytes_;
                bits.read(16);
                offset += length;
                break;
            case kTypeSelf:
                block_offset = bits.read(self_bits);
                last_self = static_cast<std::uint32_t>(block_offset);
                break;
            case kTypeParent:
                block_offset = bits.read(parent_bits);
                last_parent = block_offset;
                break;
            case kTypeSelf1:
                ++last_self;
                [[fallthrough]];
            case kTypeSelf0:
                entry.type = kTypeSelf;
                block_offset = last_self;
                break;
            case kTypeParentSelf:
                entry.type = kTypeParent;
                last_parent =
                    static_cast<std::uint64_t>(index) * units_per_hunk;
                block_offset = last_parent;
                break;
            case kTypeParent1:
                last_parent += units_per_hunk;
                [[fallthrough]];
            case kTypeParent0:
                entry.type = kTypeParent;
                block_offset = last_parent;
                break;
            default:
                error = "compressed hunk map has an unknown entry type";
                return false;
        }
        entry.length = length;
        entry.offset = block_offset;
    }
    return true;
}

bool ChdImage::decode_hunk(std::uint32_t index, std::string& error) {
    // A SELF entry repeats an earlier hunk; follow the chain once and decode
    // the referenced hunk into the cache buffer.
    std::uint32_t target = index;
    for (int guard = 0; guard < 128; ++guard) {
        const auto type = map_[target].type;
        if (type != kTypeSelf) break;
        const auto reference = map_[target].offset;
        if (reference >= map_.size() || reference >= target) {
            error = "hunk map has an out-of-range back reference";
            return false;
        }
        target = static_cast<std::uint32_t>(reference);
    }
    if (map_[target].type == kTypeSelf) {
        error = "hunk map back-reference chain is too long";
        return false;
    }

    const auto& entry = map_[target];
    if (entry.type == kTypeNone) {
        if (entry.offset != 0) {
            if (!read_at(entry.offset, buffer_.data(), hunk_bytes_)) {
                error = "cannot read hunk " + std::to_string(target);
                return false;
            }
        } else {
            std::fill(buffer_.begin(), buffer_.end(), 0);
        }
    } else if (entry.type <= 3) {
        const auto codec = compressors_[codec_at(entry.type)];
        if (entry.length > 64 * 1024 * 1024) {
            error = "hunk " + std::to_string(target) + " has an invalid size";
            return false;
        }
        std::vector<std::uint8_t> compressed(entry.length);
        if (!read_at(entry.offset, compressed.data(), compressed.size())) {
            error = "cannot read hunk " + std::to_string(target);
            return false;
        }
        bool decoded = false;
        if (codec == kCodecCdZlib || codec == kCodecCdLzma) {
            decoded = decode_cd_hunk(codec, compressed.data(),
                                     compressed.size(), buffer_.data(),
                                     buffer_.size(), error);
        } else {  // chd_flac has a different wrapper
            if (codec == kCodecCdFlac) {
                error =
                    "hunk is cdfl (FLAC) compressed; this reader only "
                    "decodes CD data hunks (cdzl/cdlz)";
                return false;
            }
            decoded = decompress_payload(codec, compressed.data(),
                                         compressed.size(), buffer_.data(),
                                         buffer_.size(), error);
        }
        if (!decoded) return false;
    } else if (entry.type == kTypeParent) {
        error = "hunk requires a parent image (delta CHD)";
        return false;
    } else {
        error = "unsupported hunk type in map";
        return false;
    }

    cached_hunk_ = index;
    return true;
}

const std::uint8_t* ChdImage::sector(std::uint32_t lba, std::string& error) {
    if (lba >= sector_count_) {
        error = "sector " + std::to_string(lba) +
                " is past the end of the image";
        return nullptr;
    }
    const auto frames_per_hunk = hunk_bytes_ / kFrameBytes;
    const auto hunk = lba / frames_per_hunk;
    const auto index = lba % frames_per_hunk;
    if (hunk >= map_.size()) {
        error = "sector " + std::to_string(lba) +
                " has no hunk entry";
        return nullptr;
    }
    if (cached_hunk_ != hunk && !decode_hunk(hunk, error)) return nullptr;
    return buffer_.data() + static_cast<std::size_t>(index) * kFrameBytes;
}

bool ChdImage::read_user_sector(std::uint32_t lba, std::uint8_t* destination,
                                std::size_t destination_size) {
    if (!open_ || destination == nullptr ||
        destination_size < kUserBytes) {
        return false;
    }
    std::string error;
    const auto* frame = sector(lba, error);
    if (frame == nullptr) {
        error_ = error;
        return false;
    }
    std::memcpy(destination, frame + kUserOffset, kUserBytes);
    return true;
}

bool ChdImage::read_sector_header(std::uint32_t lba, std::uint8_t* destination,
                                  std::size_t destination_size) {
    if (!open_ || destination == nullptr ||
        destination_size < kHeaderBytes) {
        return false;
    }
    std::string error;
    const auto* frame = sector(lba, error);
    if (frame == nullptr) {
        error_ = error;
        return false;
    }
    std::memcpy(destination, frame + kHeaderOffset, kHeaderBytes);
    return true;
}

}  // namespace r6
