#include "test_support.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "r6/chd_image.h"

// CMake points this at the source tree; a direct g++ build from the repository
// root finds the fixtures at the same relative path.
#ifndef R6_FIXTURE_DIR
#define R6_FIXTURE_DIR "tests/fixtures"
#endif

namespace {

using r6::ChdImage;

constexpr int kSectors = 40;

std::string fixture(const char* name) {
    return std::string(R6_FIXTURE_DIR) + "/" + name;
}

std::uint8_t bcd(int value) {
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

std::string repeated(const std::string& phrase) {
    std::string text;
    while (text.size() < 2048) text += phrase;
    text.resize(2048);
    return text;
}

// Mirrors make_chd_fixtures.py; the fixtures only ever contain this text.
std::string expected_text(int lba, bool duplicate) {
    if (duplicate) {
        return repeated((lba % 16) < 8 ? "r6 duplicate block a "
                                       : "r6 duplicate block b ");
    }
    char buffer[48]{};
    std::snprintf(buffer, sizeof buffer, "r6 synthetic sector %04d ", lba);
    return repeated(buffer);
}

void check_header(const std::uint8_t* header, int lba) {
    const int frames = lba + 150;
    R6_CHECK(header[0] == bcd(frames / 4500));
    R6_CHECK(header[1] == bcd((frames / 75) % 60));
    R6_CHECK(header[2] == bcd(frames % 75));
    R6_CHECK(header[3] == 0x02);
    R6_CHECK(header[4] == 0x00);
    R6_CHECK(header[5] == 0x00);
    R6_CHECK(header[6] == 0x08);
    R6_CHECK(header[7] == 0x00);
    R6_CHECK(header[8] == 0x00);
    R6_CHECK(header[9] == 0x00);
    R6_CHECK(header[10] == 0x08);
    R6_CHECK(header[11] == 0x00);
}

bool open_fixture(ChdImage& image, const char* name) {
    std::string error;
    if (image.open(fixture(name), error)) return true;
    std::printf("FAIL cannot open fixture %s: %s\n", name, error.c_str());
    ++r6::test::failure_count();
    return false;
}

void check_sectors(ChdImage& image, bool duplicate, int sectors) {
    for (int lba = 0; lba < sectors; ++lba) {
        std::uint8_t user[2048]{};
        const auto read = image.read_user_sector(
            static_cast<std::uint32_t>(lba), user, sizeof user);
        if (!read) {
            std::printf("FAIL sector %d read failed: %s\n", lba,
                        image.error().c_str());
            ++r6::test::failure_count();
            continue;
        }
        const auto expected = expected_text(lba, duplicate);
        if (std::memcmp(user, expected.data(), expected.size()) != 0) {
            std::printf("FAIL sector %d user data mismatch\n", lba);
            ++r6::test::failure_count();
        }
        std::uint8_t header[12]{};
        if (!image.read_sector_header(static_cast<std::uint32_t>(lba), header,
                                      sizeof header)) {
            std::printf("FAIL sector %d header read failed\n", lba);
            ++r6::test::failure_count();
            continue;
        }
        check_header(header, lba);
    }
}

void uncompressed_map_reads_match() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-none.chd")) return;
    R6_CHECK(image.is_open());
    R6_CHECK(image.sector_count() == static_cast<std::uint32_t>(kSectors));
    R6_CHECK(image.hunk_bytes() == 8u * 2352u + 8u * 96u);
    R6_CHECK(image.frame_bytes() == 2448u);
    check_sectors(image, false, kSectors);
}

void deflate_hunks_read_match() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-cdzl.chd")) return;
    R6_CHECK(image.sector_count() == static_cast<std::uint32_t>(kSectors));
    check_sectors(image, false, kSectors);
}

void lzma_hunks_read_match() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-cdlz.chd")) return;
    check_sectors(image, false, kSectors);
}

void mixed_codecs_read_match() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-default.chd")) return;
    check_sectors(image, false, kSectors);
}

void self_references_read_match() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-duplicate.chd")) return;
    // Hunk 1 repeats hunk 0, hunk 3 repeats hunk 2 and hunk 4 repeats hunk 0,
    // so most of this image is served through SELF map entries.
    check_sectors(image, true, kSectors);
}

// Mirrors the sparse image in make_chd_fixtures.py.
std::string sparse_text(int lba) {
    std::string body(2048, '\0');
    if (lba == 0 || lba == 7 || lba == 20 || lba == 33) {
        char buffer[48]{};
        std::snprintf(buffer, sizeof buffer, "r6 sparse sector %04d ", lba);
        std::string phrase = buffer;
        for (int index = 0; index < 64; ++index) {
            body[static_cast<std::size_t>(index)] =
                phrase[static_cast<std::size_t>(index) % phrase.size()];
        }
    }
    if (lba == 39) body.replace(2000, 11, "tail marker");
    return body;
}

void sparse_hunks_read_match() {
    // Zero-heavy sectors make LZMA emit long matches and rotate the rep
    // history, which is where a decoder state-machine bug surfaces first.
    ChdImage image;
    if (!open_fixture(image, "synthetic-sparse.chd")) return;
    for (int lba = 0; lba < kSectors; ++lba) {
        std::uint8_t user[2048]{};
        if (!image.read_user_sector(static_cast<std::uint32_t>(lba), user,
                                    sizeof user)) {
            std::printf("FAIL sparse sector %d read failed: %s\n", lba,
                        image.error().c_str());
            ++r6::test::failure_count();
            continue;
        }
        const auto expected = sparse_text(lba);
        if (std::memcmp(user, expected.data(), expected.size()) != 0) {
            std::printf("FAIL sparse sector %d mismatch\n", lba);
            ++r6::test::failure_count();
        }
    }
}

void large_hunks_read_match() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-large-hunk.chd")) return;
    R6_CHECK(image.hunk_bytes() == 78336u);
    R6_CHECK(image.hunk_bytes() > 65535u);  // 3-byte compressed lengths
    check_sectors(image, false, kSectors);
}

void data_track_reads_before_audio() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-audio.chd")) return;
    // 40 data frames, a 10-frame audio track, and chdman's padding to 52.
    R6_CHECK(image.sector_count() == 52u);
    check_sectors(image, false, kSectors);

    // The audio track is stored with cdfl and is never read by the CD
    // controller; asking for it reports the codec instead of noise.
    std::uint8_t user[2048]{};
    R6_CHECK(!image.read_user_sector(
        static_cast<std::uint32_t>(kSectors), user, sizeof user));
    R6_CHECK(image.error().find("cdfl") != std::string::npos);
}

void unsupported_codec_is_reported() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-zstd.chd")) return;
    std::uint8_t user[2048]{};
    R6_CHECK(!image.read_user_sector(0, user, sizeof user));
    R6_CHECK(image.error().find("cdzs") != std::string::npos);
}

void past_the_end_is_rejected() {
    ChdImage image;
    if (!open_fixture(image, "synthetic-none.chd")) return;
    std::uint8_t user[2048]{};
    R6_CHECK(!image.read_user_sector(static_cast<std::uint32_t>(kSectors), user,
                                     sizeof user));
}

void invalid_images_are_rejected() {
    ChdImage missing;
    std::string error;
    R6_CHECK(!missing.open(fixture("does-not-exist.chd"), error));
    R6_CHECK(!missing.is_open());
    R6_CHECK(!error.empty());

    ChdImage not_a_chd;
    error.clear();
    R6_CHECK(!not_a_chd.open(fixture("../make_chd_fixtures.py"), error));
    R6_CHECK(!error.empty());
}

}  // namespace

void run_chd_image_tests() {
    uncompressed_map_reads_match();
    deflate_hunks_read_match();
    lzma_hunks_read_match();
    mixed_codecs_read_match();
    self_references_read_match();
    sparse_hunks_read_match();
    large_hunks_read_match();
    data_track_reads_before_audio();
    unsupported_codec_is_reported();
    past_the_end_is_rejected();
    invalid_images_are_rejected();
}
