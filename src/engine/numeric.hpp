#pragma once

// Scalar reference math (docs/architecture.md, Numeric conventions).
// Everything is evaluated in double; these functions define the library's
// numeric results.

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace chromaroute::detail {

static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559,
              "IEEE-754 binary32/binary64 required");

// IEC 61966-2-1 sRGB transfer function, encoded value -> linear light.
[[nodiscard]] inline double srgbToLinear(double encoded) noexcept
{
    if (encoded <= 0.04045) {
        return encoded / 12.92; // linear segment near black
    }
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

// IEC 61966-2-1 sRGB transfer function, linear light -> encoded value.
[[nodiscard]] inline double linearToSrgb(double linear) noexcept
{
    if (linear <= 0.0031308) {
        return linear * 12.92; // linear segment near black
    }
    return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

// Unsigned normalized integers: 0..max maps to 0.0..1.0 (max = 2^bits - 1).
template <typename UInt>
[[nodiscard]] inline double decodeUnorm(UInt value) noexcept
{
    static_assert(std::is_unsigned_v<UInt>, "unsigned storage type expected");
    return value / static_cast<double>(std::numeric_limits<UInt>::max());
}

// NaN -> 0, clamp to [0, 1], then round half up: floor(v * max + 0.5), all in
// double (decision N2). ±Inf clamp like any other out-of-range value. Exact
// for binary32 inputs into 8- and 16-bit integers; for wider products the
// double rounding of v * max is the defined behavior.
template <typename UInt>
[[nodiscard]] inline UInt encodeUnorm(double value) noexcept
{
    static_assert(std::is_unsigned_v<UInt>, "unsigned storage type expected");
    constexpr UInt max = std::numeric_limits<UInt>::max();
    if (!(value > 0.0)) {
        return 0;
    }
    if (value >= 1.0) {
        return max;
    }
    return static_cast<UInt>(std::floor(value * static_cast<double>(max) + 0.5));
}

// Integers mapped to the value range [lo, hi] (decision A7): codes c_lo..c_hi
// cover it, code c meaning lo + (c - c_lo) (hi - lo) / (c_hi - c_lo). Codes
// outside c_lo..c_hi (Y'CbCr footroom and headroom) decode to lo or hi, as in
// babl (decision S8). For integer or half-integer lo and hi, (c - c_lo) (hi - lo) +
// lo (c_hi - c_lo) is exact in double, so the one division rounds correctly.
template <typename UInt>
[[nodiscard]] inline double decodeRange(UInt code, double lo, double hi, double c_lo, double c_hi) noexcept
{
    static_assert(std::is_unsigned_v<UInt>, "unsigned storage type expected");
    if (code < c_lo) {
        return lo;
    }
    if (code > c_hi) {
        return hi;
    }
    const double span = c_hi - c_lo;
    return ((code - c_lo) * (hi - lo) + lo * span) / span;
}

// The whole code range: code c means lo + c (hi - lo) / max.
template <typename UInt>
[[nodiscard]] inline double decodeRange(UInt code, double lo, double hi) noexcept
{
    return decodeRange(code, lo, hi, 0.0, static_cast<double>(std::numeric_limits<UInt>::max()));
}

// NaN and values at or below lo -> 0; at or above hi -> max; otherwise round
// half up of s / (hi - lo), like N2, where s = (value - lo) * max is evaluated
// in double. The quotient is rounded exactly: the candidate from the double
// division is checked against (code -+ 1/2) * (hi - lo), products of small
// integers that double represents exactly. s itself can be rounded (a tiny
// value minus -128, or a double with a long mantissa); for a double value that
// rounding is the defined behavior, as the product is in encodeUnorm. For a
// binary32 value the code is the exact round half up of the value itself:
// tests/exhaustive/cie_lab_exhaustive.cpp checks every binary32 input.
// With codes c_lo..c_hi: the same, with c_hi - c_lo in place of max and the
// result offset by c_lo; NaN and values at or below lo give c_lo.
template <typename UInt>
[[nodiscard]] inline UInt encodeRange(double value, double lo, double hi, double c_lo, double c_hi) noexcept
{
    static_assert(std::is_unsigned_v<UInt>, "unsigned storage type expected");
    if (!(value > lo)) {
        return static_cast<UInt>(c_lo);
    }
    if (value >= hi) {
        return static_cast<UInt>(c_hi);
    }
    const double range = hi - lo;
    const double scaled = (value - lo) * (c_hi - c_lo);
    double code = std::floor(scaled / range + 0.5);
    if ((code - 0.5) * range > scaled) {
        code -= 1.0;
    } else if ((code + 0.5) * range <= scaled) {
        code += 1.0;
    }
    return static_cast<UInt>(c_lo + code);
}

template <typename UInt>
[[nodiscard]] inline UInt encodeRange(double value, double lo, double hi) noexcept
{
    return encodeRange<UInt>(value, lo, hi, 0.0, static_cast<double>(std::numeric_limits<UInt>::max()));
}

// double -> float with IEEE-754 round-to-nearest-even, including overflow to
// infinity. A plain cast of an out-of-range value is undefined behavior in C++.
#if defined(_MSC_VER)
#pragma warning(push)
// With constant arguments MSVC's optimizer folds the final cast even for
// out-of-range values the branches above return early for (C4756).
#pragma warning(disable : 4756)
#endif
[[nodiscard]] inline float narrowToFloat(double value) noexcept
{
    constexpr double float_max = std::numeric_limits<float>::max();
    constexpr double overflow_threshold = 0x1.ffffffp127; // float_max + half an ulp: ties round to infinity
    constexpr float infinity = std::numeric_limits<float>::infinity();
    if (value > float_max) {
        return value >= overflow_threshold ? infinity : std::numeric_limits<float>::max();
    }
    if (value < -float_max) {
        return value <= -overflow_threshold ? -infinity : std::numeric_limits<float>::lowest();
    }
    return static_cast<float>(value); // in range, or NaN
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace chromaroute::detail
