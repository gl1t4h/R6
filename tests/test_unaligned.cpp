#include "test_support.h"

#include <cstdint>

#include "r6/unaligned.h"

namespace {

using r6::lwl;
using r6::lwr;
using r6::swl;
using r6::swr;

// Byte views, so the expectations are written from the ISA description rather
// than from the shift/mask arithmetic under test.
struct Word {
    std::uint8_t bytes[4];

    static Word of(std::uint32_t value) {
        Word result{};
        for (int index = 0; index < 4; ++index) {
            result.bytes[index] =
                static_cast<std::uint8_t>(value >> (index * 8));
        }
        return result;
    }

    std::uint32_t value() const {
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            result |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
        }
        return result;
    }
};

// lwl: the o+1 low bytes of the word move to the top of rt, the rest of rt is
// kept. At offset 3 the whole word is taken.
std::uint32_t reference_lwl(std::uint32_t rt, std::uint32_t aligned,
                            std::uint32_t offset) {
    Word result = Word::of(rt);
    const Word source = Word::of(aligned);
    for (std::uint32_t index = 0; index <= offset; ++index) {
        result.bytes[3 - index] = source.bytes[offset - index];
    }
    return result.value();
}

// lwr: the bytes above the offset move down into the bottom of rt, the rest of
// rt is kept. At offset 0 the whole word is taken.
std::uint32_t reference_lwr(std::uint32_t rt, std::uint32_t aligned,
                            std::uint32_t offset) {
    Word result = Word::of(rt);
    const Word source = Word::of(aligned);
    for (std::uint32_t index = 0; index + offset < 4; ++index) {
        result.bytes[index] = source.bytes[offset + index];
    }
    return result.value();
}

// swl: the top o+1 bytes of rt are stored from the offset upwards.
std::uint32_t reference_swl(std::uint32_t rt, std::uint32_t aligned,
                            std::uint32_t offset) {
    Word result = Word::of(aligned);
    const Word source = Word::of(rt);
    for (std::uint32_t index = 0; index <= offset; ++index) {
        result.bytes[index] = source.bytes[3 - offset + index];
    }
    return result.value();
}

// swr: the bottom bytes of rt are stored below the offset.
std::uint32_t reference_swr(std::uint32_t rt, std::uint32_t aligned,
                            std::uint32_t offset) {
    if (offset == 0) return rt;
    Word result = Word::of(aligned);
    const Word source = Word::of(rt);
    for (std::uint32_t index = 0; offset + index < 4; ++index) {
        result.bytes[offset + index] = source.bytes[index];
    }
    return result.value();
}

constexpr std::uint32_t kAligned = 0x11223344u;
constexpr std::uint32_t kRt = 0xAABBCCDDu;

}  // namespace

void run_unaligned_tests() {
    // All four operations at all four alignments, against byte-level
    // expectations.
    for (std::uint32_t offset = 0; offset < 4; ++offset) {
        const std::uint32_t address = 0x1000u + offset;
        R6_CHECK(lwl(kRt, kAligned, address) ==
                 reference_lwl(kRt, kAligned, offset));
        R6_CHECK(lwr(kRt, kAligned, address) ==
                 reference_lwr(kRt, kAligned, offset));
        R6_CHECK(swl(kRt, kAligned, address) ==
                 reference_swl(kRt, kAligned, offset));
        R6_CHECK(swr(kRt, kAligned, address) ==
                 reference_swr(kRt, kAligned, offset));
    }

    // The case that broke the loader: an lwl at offset 1 followed by an lwr at
    // offset 2 must load the four bytes at that address and keep nothing of the
    // previous rt. Here rt holds 0x000C0000, the stale value that used to leak
    // into the directory record's extent.
    {
        std::uint32_t value = 0x000C0000u;
        value = lwl(value, 0x00010000u, 0x1000u + 1);
        value = lwr(value, 0x00160001u, 0x1000u + 2);
        R6_CHECK(value == 0x00000016u);
    }

    // The lwl/lwr pair used by the loader's record copy: lwl at offset 1 then
    // lwr at offset 2 must rebuild the four bytes at that address with no
    // stale rt bits (this is the exact 0x000C0016 failure).
    for (std::uint32_t offset = 0; offset + 1 < 4; ++offset) {
        const Word big = Word::of(0xDEADBEEFu);
        const std::uint32_t address = 0x3000u + offset;
        std::uint32_t value = kAligned;
        value = lwl(value, big.value(), address);
        value = lwr(value, big.value(), address + 1);
        const std::uint32_t expected =
            reference_lwr(reference_lwl(kAligned, big.value(), offset),
                          big.value(), offset + 1);
        R6_CHECK(value == expected);
    }
}
