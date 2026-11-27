// Pulls one video frame out of an STR stream on a PlayStation disc image and
// decodes it, with no emulated console in between.
//
// The movie player on the console reads a sector, hands the 2016 bytes after the
// 32-byte record header to the macroblock decoder and uploads the result. This
// does the same two steps from the image itself, which is what a native port
// needs for its cutscenes: the stream layout and the decoder, without the drive,
// the interrupts or the ring the guest's driver builds.
//
// The stream is not the 16-bit run/level form the port's MdecDevice reads. It is
// the same intra coding as MPEG-1, bit-packed:
//   - every 16-bit unit is byte-swapped, then read most significant bit first;
//   - 4 bytes of preamble, then a 16-bit quantiser scale and a 16-bit version;
//   - per macroblock, in column-major order, six blocks: Cr, Cb, Y0, Y1, Y2, Y3;
//   - a block starts with a signed 10-bit DC whose value is doubled and used as
//     is (no offset); the picture is centred by adding 128 after the inverse
//     transform;
//   - AC coefficients are MPEG-1 run/level codes with a sign bit each, and the
//     run counts from the first AC coefficient (scan index 1), with `10` as end
//     of block and `000001` as escape.
// Checked against libavcodec's MDEC decoder: frame 61 of ATVILOGO decoded here
// and by ffmpeg agree to a mean absolute error of 1.9 on luma.
//
// Build (same toolchain as the port):
//   cl /nologo /std:c++20 /EHsc /O2 /W3 /Fe:str-frame.exe tools\str-frame.cpp
// Run:
//   str-frame.exe "Spider-Man (USA).bin" 128304 60 out.ppm
//     arg 2: LBA to start scanning at
//     arg 3: which video frame to decode, counting the frames found from there
//     arg 4: output .ppm
//     arg 5, 6: coded width and height, defaulting to the record's 320x240

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kSectorBytes = 2352;
constexpr std::size_t kUserOffset = 24;    // MODE2/2352: sync, header, subheader
constexpr std::size_t kUserBytes = 2048;
constexpr std::size_t kRecordHeader = 32;  // STR record header in the user data
constexpr std::size_t kChunkBytes = kUserBytes - kRecordHeader;

// The quantisation matrices. The stream carries its own scale but not the
// matrix, so this is the MPEG-1 intra matrix every PlayStation encoder used.
constexpr std::uint8_t kQuant[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// Zigzag order: scan position to raster position, DC first.
constexpr std::uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

#include "str-vlc.inc"

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Bits arrive as 16-bit units in the other byte order, most significant bit
// first. Reading a byte at a time out of a swapped copy keeps that exact.
class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t>& payload) {
        bytes_ = payload;
        for (std::size_t at = 0; at + 1 < bytes_.size(); at += 2) {
            std::swap(bytes_[at], bytes_[at + 1]);
        }
    }

    bool ok() const { return position_ <= bytes_.size() * 8; }
    std::size_t position() const { return position_; }
    std::size_t size() const { return bytes_.size() * 8; }

    std::uint32_t bits(int count) {
        std::uint32_t value = 0;
        for (int index = 0; index < count; ++index) {
            const std::size_t byte = position_ >> 3;
            const std::uint8_t bit =
                byte < bytes_.size()
                    ? static_cast<std::uint8_t>(
                          (bytes_[byte] >> (7 - (position_ & 7))) & 1u)
                    : 0u;
            value = (value << 1) | bit;
            ++position_;
        }
        return value;
    }

    std::int32_t signed_bits(int count) {
        const std::uint32_t value = bits(count);
        const std::uint32_t sign = 1u << (count - 1);
        return (value & sign) != 0
                   ? static_cast<std::int32_t>(value) - (1 << count)
                   : static_cast<std::int32_t>(value);
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t position_ = 0;
};

struct Frame {
    long number = -1;
    std::uint16_t slices = 0;
    std::uint32_t data_size = 0;
    std::vector<std::uint8_t> payload;
};

bool read_sector(std::ifstream& bin, long lba, std::vector<std::uint8_t>& out) {
    out.assign(kSectorBytes, 0);
    bin.clear();
    bin.seekg(static_cast<std::streamoff>(lba) * kSectorBytes);
    if (!bin.read(reinterpret_cast<char*>(out.data()), kSectorBytes)) return false;
    return true;
}

// Collects video records from `first_lba` on, grouping them into frames by the
// record header's frame number. Audio sectors interleaved in the stream carry
// submode bit 2 and are not part of a frame.
std::vector<Frame> read_frames(std::ifstream& bin, long first_lba, long wanted) {
    std::vector<std::uint8_t> sector;
    long current = -1;
    long seen = 0;  // frames met so far
    bool collecting = false;
    Frame frame;
    for (long lba = first_lba; lba < first_lba + 200000; ++lba) {
        if (!read_sector(bin, lba, sector)) break;
        const std::uint8_t* user = sector.data() + kUserOffset;
        if ((user[6] & 0x04u) != 0) continue;  // XA audio
        if (le16(user) != 0x0160) continue;    // not a video record
        const long number = static_cast<long>(le32(user + 8));
        if (number != current) {
            if (collecting && !frame.payload.empty()) break;
            current = number;
            const bool mine = (seen == wanted);
            ++seen;
            collecting = mine;
            if (mine) {
                frame = Frame{};
                frame.number = number;
                frame.slices = le16(user + 6);
                frame.data_size = le32(user + 0x0C);
            }
            if (std::getenv("STR_FRAME_TRACE") != nullptr) {
                std::fprintf(stderr, "  lba %ld frame %ld mine %d\n", lba, number,
                             mine ? 1 : 0);
            }
            if (!mine) continue;
        }
        if (!collecting) continue;
        frame.payload.insert(frame.payload.end(), user + kRecordHeader,
                             user + kUserBytes);
    }
    if (!collecting || frame.payload.empty()) return {};
    return {frame};
}

// The inverse transform, in floating point: this is a viewer, not the console, so
// the exact integer path the hardware uses does not matter here.
struct Idct {
    double cos[8][8];

    Idct() {
        constexpr double kPi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u) {
            const double scale = u == 0 ? std::sqrt(0.5) : 1.0;
            for (int x = 0; x < 8; ++x) {
                cos[u][x] = scale * std::cos((2 * x + 1) * u * kPi / 16.0);
            }
        }
    }

    void run(const int block[64], std::uint8_t out[64]) const {
        double rows[8][8];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                double sum = 0.0;
                for (int u = 0; u < 8; ++u) sum += cos[u][x] * block[y * 8 + u];
                rows[y][x] = sum;
            }
        }
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                double sum = 0.0;
                for (int v = 0; v < 8; ++v) sum += cos[v][y] * rows[v][x];
                const double value = std::round(0.25 * sum) + 128.0;
                out[y * 8 + x] = static_cast<std::uint8_t>(
                    std::clamp(value, 0.0, 255.0));
            }
        }
    }
};

struct DecodeStats {
    unsigned qscale = 0;
    unsigned version = 0;
    std::size_t macroblocks = 0;
    std::size_t damaged = 0;
    std::size_t bits_used = 0;
    std::size_t bits_total = 0;
};

// One block: the 10-bit DC, then run/level codes until end of block or until the
// scan runs past the last coefficient.
bool decode_block(BitReader& reader, int block[64], int qscale, DecodeStats& stats) {
    for (int index = 0; index < 64; ++index) block[index] = 0;
    block[0] = 2 * reader.signed_bits(10);
    int scan = 1;  // runs count from the first AC coefficient
    while (true) {
        const RlCode* found = nullptr;
        std::uint32_t code = 0;
        for (int length = 1; length <= 16 && found == nullptr; ++length) {
            code = (code << 1) | reader.bits(1);
            for (const auto& entry : kRlCodes) {
                if (entry.bits == length && entry.code == code) {
                    found = &entry;
                    break;
                }
            }
        }
        if (found == nullptr) {
            ++stats.damaged;
            return false;
        }
        if (found->kind == 2) return true;  // end of block
        if (found->kind == 1) {             // escape: run and level as literals
            scan += static_cast<int>(reader.bits(6)) + 1;
            const int level = reader.signed_bits(10);
            if (scan > 63) {
                ++stats.damaged;
                return false;
            }
            const int at = kZigzag[scan];
            block[at] = (level * qscale * kQuant[at]) >> 3;
            continue;
        }
        scan += found->run;
        if (scan > 63) {
            ++stats.damaged;
            return false;
        }
        const int at = kZigzag[scan];
        const int magnitude = (found->level * qscale * kQuant[at]) >> 3;
        block[at] = reader.bits(1) != 0 ? -magnitude : magnitude;
    }
}

void write_ppm(const std::string& path, int width, int height,
               const std::vector<std::uint8_t>& rgb) {
    std::ofstream ppm(path, std::ios::binary);
    ppm << "P6\n" << width << " " << height << "\n255\n";
    ppm.write(reinterpret_cast<const char*>(rgb.data()),
              static_cast<std::streamsize>(rgb.size()));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: str-frame <disc.bin> <first-lba> <frame> <out.ppm>"
                     " [width] [height]\n");
        return 2;
    }
    const std::string bin_path = argv[1];
    const long first_lba = std::strtol(argv[2], nullptr, 0);
    const long wanted = std::strtol(argv[3], nullptr, 0);
    const std::string out_path = argv[4];
    const int width = argc > 5 ? std::atoi(argv[5]) : 320;
    const int height = argc > 6 ? std::atoi(argv[6]) : 240;
    if (width < 16 || height < 16 || width % 16 != 0 || height % 16 != 0) {
        std::fprintf(stderr, "coded size must be a multiple of 16\n");
        return 2;
    }

    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) {
        std::fprintf(stderr, "cannot open %s\n", bin_path.c_str());
        return 1;
    }
    auto frames = read_frames(bin, first_lba, wanted);
    if (frames.empty()) {
        std::fprintf(stderr, "no video frame %ld found from LBA %ld\n", wanted,
                     first_lba);
        return 1;
    }
    auto& frame = frames.front();
    // The record header carries the coded size of the frame; the rest of the
    // slices is padding, and padding is not harmless: a zero word reads as a
    // valid code, so decoding it invents macroblocks the encoder never wrote.
    if (frame.data_size > 0 && frame.data_size < frame.payload.size()) {
        frame.payload.resize(frame.data_size);
    }
    std::printf("frame %ld: %u slice(s), %u byte(s) of data, %zu carried\n",
                frame.number, frame.slices, frame.data_size,
                frame.payload.size());

    BitReader reader(frame.payload);
    reader.bits(32);  // preamble: typically the frame's own header word
    DecodeStats stats;
    stats.qscale = reader.bits(16);
    stats.version = reader.bits(16);
    stats.bits_total = reader.size();

    const int mb_wide = width / 16;
    const int mb_high = height / 16;
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(width) * height, 128);
    std::vector<std::uint8_t> blue(static_cast<std::size_t>(width / 2) *
                                       (height / 2),
                                   128);
    std::vector<std::uint8_t> red = blue;
    const Idct idct;
    std::uint8_t samples[64];

    // Macroblock order is column major: the encoder walks down a column before
    // moving right, which is why a row-major read looks like a scrambled frame.
    for (int mb_x = 0; mb_x < mb_wide; ++mb_x) {
        for (int mb_y = 0; mb_y < mb_high; ++mb_y) {
            if (!reader.ok()) break;
            // Cr, Cb, then the four luma quadrants.
            for (const int component : {5, 4, 0, 1, 2, 3}) {
                int block[64];
                decode_block(reader, block, static_cast<int>(stats.qscale), stats);
                idct.run(block, samples);
                if (component < 4) {
                    const int quadrant_x = component % 2;
                    const int quadrant_y = component / 2;
                    const int ox = mb_x * 16 + quadrant_x * 8;
                    const int oy = mb_y * 16 + quadrant_y * 8;
                    for (int y = 0; y < 8; ++y) {
                        for (int x = 0; x < 8; ++x) {
                            luma[static_cast<std::size_t>(oy + y) * width + ox + x] =
                                samples[y * 8 + x];
                        }
                    }
                } else {
                    auto& plane = component == 5 ? red : blue;
                    for (int y = 0; y < 8; ++y) {
                        for (int x = 0; x < 8; ++x) {
                            plane[(static_cast<std::size_t>(mb_y * 8 + y)) *
                                      (width / 2) +
                                  mb_x * 8 + x] = samples[y * 8 + x];
                        }
                    }
                }
            }
            ++stats.macroblocks;
        }
    }
    stats.bits_used = reader.position();

    std::vector<std::uint8_t> rgb(
        static_cast<std::size_t>(width) * height * 3, 0);
    std::size_t lit = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double luma_at = luma[static_cast<std::size_t>(y) * width + x];
            const double cb = static_cast<double>(
                                  blue[(static_cast<std::size_t>(y / 2)) *
                                           (width / 2) +
                                       x / 2]) -
                              128.0;
            const double cr = static_cast<double>(
                                  red[(static_cast<std::size_t>(y / 2)) *
                                          (width / 2) +
                                      x / 2]) -
                              128.0;
            const double r = luma_at + 1.402 * cr;
            const double g = luma_at - 0.344136 * cb - 0.714136 * cr;
            const double b = luma_at + 1.772 * cb;
            const std::size_t at =
                (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[at] = static_cast<std::uint8_t>(std::clamp(r, 0.0, 255.0));
            rgb[at + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.0, 255.0));
            rgb[at + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.0, 255.0));
            if (rgb[at] || rgb[at + 1] || rgb[at + 2]) ++lit;
        }
    }

    std::printf(
        "  qscale %u, version %u, %zu of %zu macroblock(s), %zu damaged, "
        "%zu of %zu bit(s) used, %zu lit pixel(s)\n",
        stats.qscale, stats.version, stats.macroblocks,
        static_cast<std::size_t>(mb_wide) * mb_high, stats.damaged,
        stats.bits_used, stats.bits_total, lit);
    write_ppm(out_path, width, height, rgb);
    std::printf("  wrote %s\n", out_path.c_str());
    return 0;
}
