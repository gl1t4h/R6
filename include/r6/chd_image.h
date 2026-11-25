#pragma once

// Reader for MAME CHD ("Compressed Hunks of Data") version 5 CD images.
//
// R6 loads raw BIN/CUE discs by seeking to `lba * 2352`.  A CHD stores the
// same sectors inside compressed hunks, so this class presents the two raw
// slices the CD controller actually uses: the 2048 user bytes at offset 24
// of each sector, and the 12-byte address/mode/subheader block at offset 12.
//
// Supported: uncompressed hunks, cdzl (deflate), cdlz (LZMA + deflate
// subcode), and the plain zlib/lzma codecs.  A hunk compressed with FLAC
// (cdfl) is only produced for CD audio tracks, which the emulated CD
// controller does not read; requesting one reports an error instead of
// returning silent garbage.  Delta CHDs with a parent are rejected.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace r6 {

class ChdImage {
public:
    ChdImage() = default;
    ChdImage(const ChdImage&) = delete;
    ChdImage& operator=(const ChdImage&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);

    bool is_open() const { return open_; }
    std::uint32_t sector_count() const { return sector_count_; }
    std::uint32_t hunk_bytes() const { return hunk_bytes_; }
    std::uint32_t frame_bytes() const { return frame_bytes_; }
    const std::string& error() const { return error_; }

    bool read_user_sector(std::uint32_t lba, std::uint8_t* destination,
                          std::size_t destination_size);
    bool read_sector_header(std::uint32_t lba, std::uint8_t* destination,
                            std::size_t destination_size);

private:
    struct MapEntry {
        std::uint8_t type{0};
        std::uint32_t length{0};
        std::uint64_t offset{0};
    };

    std::uint8_t codec_at(std::uint8_t type) const;
    bool load_map(std::string& error);
    bool load_compressed_map(std::string& error);
    bool decode_hunk(std::uint32_t index, std::string& error);
    bool decode_cd(const std::uint8_t* source, std::size_t source_size,
                   std::size_t destination_size, std::uint8_t* destination,
                   std::string& error);
    bool read_at(std::uint64_t offset, std::uint8_t* destination,
                 std::size_t size);
    const std::uint8_t* sector(std::uint32_t lba, std::string& error);

    std::ifstream file_;
    bool open_{false};
    std::uint32_t hunk_bytes_{0};
    std::uint32_t frame_bytes_{0};
    std::uint32_t sector_count_{0};
    std::uint32_t hunk_count_{0};
    std::uint64_t logical_bytes_{0};
    std::uint64_t map_offset_{0};
    std::uint32_t compressors_[4]{0, 0, 0, 0};
    std::vector<MapEntry> map_;
    std::vector<std::uint8_t> buffer_;
    std::uint32_t cached_hunk_{0xFFFFFFFFu};
    std::string error_;
};

}  // namespace r6
