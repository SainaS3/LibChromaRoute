#pragma once

// IEEE-754 binary16 ("half") conversions, independent of hardware support.
// Encoding rounds directly from double (never via float, which would round
// twice), to nearest with ties to even, including subnormals and overflow to
// infinity. NaN stays NaN (quiet) with the top payload bits kept.

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chromaroute::detail {

[[nodiscard]] inline double decodeHalf(std::uint16_t half) noexcept
{
    const bool negative = (half & 0x8000u) != 0;
    const int exponent = (half >> 10) & 0x1F;
    const int mantissa = half & 0x3FF;
    double magnitude;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<double>(mantissa), -24); // zero or subnormal
    } else if (exponent == 31) {
        if (mantissa != 0) {
            const std::uint64_t nan = 0x7FF8000000000000ull | (static_cast<std::uint64_t>(mantissa) << 42);
            return std::bit_cast<double>(nan | (negative ? 0x8000000000000000ull : 0));
        }
        magnitude = std::numeric_limits<double>::infinity();
    } else {
        magnitude = std::ldexp(static_cast<double>(1024 + mantissa), exponent - 25);
    }
    return negative ? -magnitude : magnitude;
}

[[nodiscard]] inline std::uint16_t encodeHalf(double value) noexcept
{
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 48) & 0x8000u);
    const int exponent = static_cast<int>((bits >> 52) & 0x7FF);
    const std::uint64_t mantissa = bits & ((1ull << 52) - 1);

    if (exponent == 0x7FF) {
        if (mantissa != 0) {
            return static_cast<std::uint16_t>(sign | 0x7E00u | ((mantissa >> 42) & 0x1FFu)); // quiet NaN
        }
        return static_cast<std::uint16_t>(sign | 0x7C00u); // infinity
    }
    const int unbiased = exponent - 1023;
    if (exponent == 0 || unbiased < -26) {
        return sign; // below a quarter of the smallest subnormal: rounds to zero
    }
    if (unbiased > 15) {
        return static_cast<std::uint16_t>(sign | 0x7C00u); // overflow
    }

    // Significand with its implicit bit, as an integer: value = significand * 2^(unbiased - 52).
    const std::uint64_t significand = mantissa | (1ull << 52);
    // Normal halves keep 11 significant bits; subnormals keep fewer.
    const int shift = unbiased >= -14 ? 42 : 42 + (-14 - unbiased);
    std::uint64_t kept = significand >> shift;
    const std::uint64_t remainder = significand & ((1ull << shift) - 1);
    const std::uint64_t halfway = 1ull << (shift - 1);
    if (remainder > halfway || (remainder == halfway && (kept & 1) != 0)) {
        ++kept; // may carry into the exponent, up to infinity: still the correct encoding
    }
    std::uint32_t encoded;
    if (unbiased >= -14) {
        encoded = static_cast<std::uint32_t>((unbiased + 14) << 10) + static_cast<std::uint32_t>(kept);
    } else {
        encoded = static_cast<std::uint32_t>(kept); // subnormal field; a carry yields the smallest normal
    }
    return static_cast<std::uint16_t>(sign | encoded);
}

} // namespace chromaroute::detail
