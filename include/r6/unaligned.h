#pragma once

#include <cstdint>

namespace r6 {

// The four unaligned load/store halves of the MIPS II ISA as the PS1's
// little-endian bus implements them. `aligned` is the whole word containing
// `address`; `address & 3` selects which bytes are taken or replaced.
//
// The interpreter and the generated translation both call these, so the byte
// tables live in exactly one place and cannot drift apart again.
inline std::uint32_t lwl(std::uint32_t rt, std::uint32_t aligned,
                         std::uint32_t address) {
    const auto shift = (address & 3u) * 8u;
    const auto mask = 0x00FFFFFFu >> shift;
    return (rt & mask) | (aligned << (24u - shift));
}

inline std::uint32_t lwr(std::uint32_t rt, std::uint32_t aligned,
                         std::uint32_t address) {
    const auto shift = (address & 3u) * 8u;
    if (shift == 0) return aligned;
    const auto mask = 0xFFFFFFFFu << (32u - shift);
    return (rt & mask) | (aligned >> shift);
}

inline std::uint32_t swl(std::uint32_t rt, std::uint32_t aligned,
                         std::uint32_t address) {
    const auto shift = (address & 3u) * 8u;
    if (shift == 24) return rt;
    const auto mask = 0xFFFFFF00u << shift;
    return (aligned & mask) | (rt >> (24u - shift));
}

inline std::uint32_t swr(std::uint32_t rt, std::uint32_t aligned,
                         std::uint32_t address) {
    const auto shift = (address & 3u) * 8u;
    if (shift == 0) return rt;
    const auto mask = 0xFFFFFFFFu >> (32u - shift);
    return (aligned & mask) | (rt << shift);
}

}  // namespace r6
