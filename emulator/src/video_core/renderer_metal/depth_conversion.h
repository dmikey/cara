// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef __METAL_VERSION__
#include <bit>
#include <cstdint>
#endif

namespace PicaMetalDepth {

#ifdef __METAL_VERSION__
using Word = uint;
#else
using Word = std::uint32_t;
#endif

inline Word LeadingZeros(Word value) {
#ifdef __METAL_VERSION__
    return metal::clz(value);
#else
    return std::countl_zero(value);
#endif
}

// Return the binary32 representation of raw / (2^depth_bits - 1).
// The argument must fit the selected 16- or 24-bit depth field.
inline Word Normalize(Word raw, Word depth_bits) {
    if (raw == 0)
        return 0;

    const Word high_bit = 31u - LeadingZeros(raw);
    const Word mantissa = raw << (23u - high_bit);
    const Word maximum = (1u << depth_bits) - 1u;
    // Scale the normalized integer by 2^depth_bits / maximum.
    // maximum is odd, so rounding this quotient never encounters a tie.
    const Word quotient = (mantissa + maximum / 2u) / maximum;
    const Word rounded = mantissa + quotient;
    Word exponent = 127u + high_bit - depth_bits;
    if (rounded >= 0x1000000u)
        ++exponent;
    return (exponent << 23u) | (rounded & 0x7fffffu);
}

// Return round(depth * (2^depth_bits - 1)) for finite binary32 depth in
// [0, 1]. This rounds half up as in the existing replay's std::lround.
inline Word Quantize(Word float_bits, Word depth_bits) {
    const Word exponent = (float_bits >> 23u) & 0xffu;
    if (exponent < 102u) {
        // Even at 24 bits the magnitude is strictly below half a unit.
        return 0;
    }

    const Word mantissa = 0x800000u | (float_bits & 0x7fffffu);
    const Word left_low = mantissa << depth_bits;
    const Word low = left_low - mantissa;
    const Word high = (mantissa >> (32u - depth_bits)) - Word(left_low < mantissa);
    const Word shift = 150u - exponent;
    if (shift < 32u) {
        const Word quotient = (high << (32u - shift)) | (low >> shift);
        return quotient + ((low >> (shift - 1u)) & 1u);
    }
    if (shift == 32u)
        return high + (low >> 31u);

    return (high >> (shift - 32u)) + ((high >> (shift - 33u)) & 1u);
}

inline bool IsValidDepth(Word bits) {
    return bits <= 0x3f800000u || bits == 0x80000000u;
}

} // namespace PicaMetalDepth
