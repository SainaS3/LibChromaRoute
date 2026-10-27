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
