// Pulls one video frame out of an STR stream on a PlayStation disc image and
// decodes it with the port's MDEC model, with no emulated console in between.
//
// The movie player on the console reads a sector, hands the 2016 bytes after the
// 32-byte record header to the macroblock decoder and uploads the result. This
// does the same three steps from the image itself, which is what a native port
// needs for its cutscenes: the stream layout and the decoder, without the drive,
// the interrupts or the ring the guest's driver builds.
//
// Build (same toolchain as the port):
//   cl /nologo /std:c++20 /EHsc /O2 /W3 /Iinclude /Fe:str-frame.exe tools\str-frame.cpp
// Run:
//   str-frame.exe "Spider-Man (USA).bin" 128304 0 out.ppm
//     arg 2: LBA to start scanning at
//     arg 3: which video frame to decode, counting the frames found from there
//     arg 4: output .ppm (a -24bit variant is written for the 24-bit pass)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "r6/mdec.h"

namespace {

constexpr std::size_t kSectorBytes = 2352;
constexpr std::size_t kUserOffset = 24;    // MODE2/2352: sync, header, subheader
constexpr std::size_t kUserBytes = 2048;
constexpr std::size_t kRecordHeader = 32;  // STR record header in the user data

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// The encoder's quantisation matrices. The decoder multiplies each coefficient
// by these; a stream that carries its own tables would send them through the
// same command word.
constexpr std::uint8_t kQuant[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

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
    long seen = 0;      // frames met so far
    bool collecting = false;
    Frame frame;
    for (long lba = first_lba; lba < first_lba + 200000; ++lba) {
        if (!read_sector(bin, lba, sector)) break;
        const std::uint8_t* user = sector.data() + kUserOffset;
        if ((user[6] & 0x04u) != 0) continue;  // XA audio
        if (le16(user) != 0x0160) continue;    // not a video record
        const long number = static_cast<long>(le32(user + 8));
        const std::uint16_t slice = le16(user + 4);
        if (number != current) {
            if (collecting) {
                // Reached the frame after the wanted one, unless the wanted
                // frame has no payload yet (a stream that starts mid-frame).
                if (!frame.payload.empty()) break;
            }
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
                std::fprintf(stderr, "  lba %ld frame %ld slice %u mine %d\n", lba,
                             number, slice, mine ? 1 : 0);
            }
            if (!mine) continue;
        }
        if (!collecting) continue;
        frame.payload.insert(frame.payload.end(), user + kRecordHeader,
                             user + kUserBytes);
        // Keep scanning past the frame; the next frame number ends this one and
        // the caller only keeps what it asked for.
        (void)slice;
    }
    if (!collecting || frame.payload.empty()) return {};
    return {frame};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: str-frame <disc.bin> <first-lba> <frame> <out.ppm>\n");
        return 2;
    }
    const std::string bin_path = argv[1];
    const long first_lba = std::strtol(argv[2], nullptr, 0);
    const long wanted = std::strtol(argv[3], nullptr, 0);
    const std::string out_path = argv[4];

    std::ifstream bin(bin_path, std::ios::binary);
    if (!bin) {
        std::fprintf(stderr, "cannot open %s\n", bin_path.c_str());
        return 1;
    }
    // read_frames returns the frame that was asked for, and nothing else.
    auto frames = read_frames(bin, first_lba, wanted);
    if (frames.empty()) {
        std::fprintf(stderr, "no video frame %ld found from LBA %ld\n", wanted,
                     first_lba);
        return 1;
    }
    auto& frame = frames.front();
    // The record header carries the coded size of the frame. The rest of the
    // slices is padding, and padding bytes are not harmless: a zero word reads as
    // a valid "run 0, level 0" code, so decoding it invents macroblocks that the
    // encoder never wrote.
    if (frame.data_size > 0 && frame.data_size < frame.payload.size()) {
        frame.payload.resize(frame.data_size);
    }
    std::printf("frame %ld: %u slice(s), %u byte(s) of data, %zu carried\n",
                frame.number, frame.slices, frame.data_size,
                frame.payload.size());

    // The decoder consumes 16-bit run/level codes, so the byte stream becomes
    // consecutive big-endian halfwords packed into the 32-bit words the DMA
    // would have delivered.
    const bool little_endian = argc > 5 && std::string(argv[5]) == "le";
    const bool swap_halves = argc > 6 && std::string(argv[6]) == "swap";
    std::vector<std::uint32_t> words;
    for (std::size_t at = 0; at + 3 < frame.payload.size(); at += 4) {
        std::uint32_t first = little_endian ? le16(&frame.payload[at])
                                            : be16(&frame.payload[at]);
        std::uint32_t second = little_endian ? le16(&frame.payload[at + 2])
                                             : be16(&frame.payload[at + 2]);
        if (swap_halves) std::swap(first, second);
        words.push_back(first | (second << 16));
    }
    if (words.empty()) {
        std::fprintf(stderr, "frame has no complete word\n");
        return 1;
    }
    std::printf("  %zu word(s) of bitstream\n", words.size());

    // The record header states 320x240, but a stream only codes as many
    // macroblocks as it needs, so both are overridable for trying a coded size
    // that is smaller than the display size.
    const int width = argc > 7 ? std::atoi(argv[7]) : 320;
    const int height = argc > 8 ? std::atoi(argv[8]) : 240;
    const std::size_t macroblocks =
        static_cast<std::size_t>(width / 16) * static_cast<std::size_t>(height / 16);

    for (int depth = 3; depth >= 0; --depth) {
        r6::MdecDevice mdec;
        // 15-bit output is depth 3 and 24-bit is depth 2; both are signed-range
        // pixels here, which is what a video frame uses.
        mdec.write_command((2u << 29) | 1u);  // quant tables, luma and chroma
        for (int table = 0; table < 2; ++table) {
            for (int index = 0; index < 64; index += 4) {
                std::uint32_t word = 0;
                for (int byte = 0; byte < 4; ++byte) {
                    word |= static_cast<std::uint32_t>(kQuant[index + byte])
                            << (byte * 8);
                }
                mdec.push_word(word);
            }
        }
        mdec.write_command((1u << 29) |
                           (static_cast<std::uint32_t>(depth) << 27) |
                           (1u << 26) |
                           static_cast<std::uint32_t>(words.size() & 0xFFFFu));
        for (const auto word : words) mdec.push_word(word);

        std::vector<std::uint32_t> output;
        while (mdec.pending_words() > 0) output.push_back(mdec.read_word());

        std::vector<std::uint8_t> rgb(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3,
            0);
        std::size_t nonblack = 0;
        if (depth == 3) {
            std::size_t pixel = 0;
            for (const auto word : output) {
                for (int half = 0; half < 2; ++half) {
                    const auto value =
                        static_cast<std::uint16_t>(word >> (half * 16));
                    const int r = (value & 0x1F) << 3;
                    const int g = ((value >> 5) & 0x1F) << 3;
                    const int b = ((value >> 10) & 0x1F) << 3;
                    if ((pixel + 1) * 3 > rgb.size()) break;
                    rgb[pixel * 3] = static_cast<std::uint8_t>(r);
                    rgb[pixel * 3 + 1] = static_cast<std::uint8_t>(g);
                    rgb[pixel * 3 + 2] = static_cast<std::uint8_t>(b);
                    if (r || g || b) ++nonblack;
                    ++pixel;
                }
            }
        } else if (depth <= 1) {
            // Mono output is one block per quadrant of the macroblock, each 64
            // samples of one byte at depth 1, so the words are just a byte
            // stream laid out TL, TR, BL, BR.
            std::vector<std::uint8_t> bytes;
            bytes.reserve(output.size() * 4);
            for (const auto word : output) {
                bytes.push_back(static_cast<std::uint8_t>(word));
                bytes.push_back(static_cast<std::uint8_t>(word >> 8));
                bytes.push_back(static_cast<std::uint8_t>(word >> 16));
                bytes.push_back(static_cast<std::uint8_t>(word >> 24));
            }
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                const std::size_t block = index / 64;
                const std::size_t within = index % 64;
                const std::size_t mbx = (block / 4) % (width / 16);
                const std::size_t mby = (block / 4) / (width / 16);
                const std::size_t quadrant = block % 4;
                const std::size_t px = mbx * 16 + (quadrant % 2) * 8 + within % 8;
                const std::size_t py = mby * 16 + (quadrant / 2) * 8 + within / 8;
                if (px >= static_cast<std::size_t>(width) ||
                    py >= static_cast<std::size_t>(height))
                    continue;
                const auto value = bytes[index];
                const std::size_t at = (py * width + px) * 3;
                rgb[at] = value;
                rgb[at + 1] = value;
                rgb[at + 2] = value;
                if (value) ++nonblack;
            }
        } else {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(output.size() * 4);
            for (const auto word : output) {
                bytes.push_back(static_cast<std::uint8_t>(word));
                bytes.push_back(static_cast<std::uint8_t>(word >> 8));
                bytes.push_back(static_cast<std::uint8_t>(word >> 16));
                bytes.push_back(static_cast<std::uint8_t>(word >> 24));
            }
            const std::size_t pixels =
                std::min(bytes.size() / 3, rgb.size() / 3);
            for (std::size_t index = 0; index < pixels; ++index) {
                const int b = bytes[index * 3];
                const int g = bytes[index * 3 + 1];
                const int r = bytes[index * 3 + 2];
                rgb[index * 3] = static_cast<std::uint8_t>(r);
                rgb[index * 3 + 1] = static_cast<std::uint8_t>(g);
                rgb[index * 3 + 2] = static_cast<std::uint8_t>(b);
                if (b || g || r) ++nonblack;
            }
        }
        std::printf(
            "  depth %d: %zu of %zu macroblock(s) decoded, %zu word(s) in, "
            "%zu non-black pixel(s)\n",
            depth,
            static_cast<std::size_t>(mdec.macroblocks()),
            macroblocks, static_cast<std::size_t>(mdec.words_in()), nonblack);

        std::string path = out_path;
        {
            const auto dot = path.rfind('.');
            const std::string tag = "-d" + std::to_string(depth) +
                                    (little_endian ? "-le" : "-be") +
                                    (swap_halves ? "-swap" : "");
            if (dot != std::string::npos) path.insert(dot, tag);
            else path += tag;
        }
        std::ofstream ppm(path, std::ios::binary);
        ppm << "P6\n" << width << " " << height << "\n255\n";
        ppm.write(reinterpret_cast<const char*>(rgb.data()),
                  static_cast<std::streamsize>(rgb.size()));
        std::printf("  wrote %s\n", path.c_str());
    }
    return 0;
}
