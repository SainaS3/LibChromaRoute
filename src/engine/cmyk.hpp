#pragma once

// Naive CMYK (decision S5): the reference catalogue's reference formula, on
// linear RGB with sRGB primaries (scRGB) whatever the format's space. With inks
// c = 1 - R, m = 1 - G, y = 1 - B, key = min(1, c, m, y), each ink becomes
// (c - key) / (1 - key) when key < 1 and stays as it is otherwise (rich black:
// C = M = Y = K = 1 for R = G = B = 0). In the inverted form (1 = no ink) and
// with w = max(R, G, B) that is exactly (R / w, G / w, B / w, w) for w > 0 and
// (R, G, B, 0) otherwise, and it is evaluated in that form, which does not
// cancel near black. Back, R = c' k' in the inverted form, so every color with
// w > 0 round-trips in real arithmetic, and one with w <= 0 comes back black.
// A NaN or infinite component gives NaN in every component.

#include "engine/color_math.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace chromaroute::detail {

// C, M, Y, K in the inverted form: 1 = no ink.
using InvertedCmyk = std::array<double, 4>;

[[nodiscard]] inline InvertedCmyk invertedCmykFromRgb(const Vector3& rgb) noexcept
{
    if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2])) {
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan, nan};
    }
    const double w = std::fmax(rgb[0], std::fmax(rgb[1], rgb[2]));
    if (!(w > 0.0)) {
        return {rgb[0], rgb[1], rgb[2], 0.0}; // key 1: the inks stay 1 - R, 1 - G, 1 - B
    }
    return {rgb[0] / w, rgb[1] / w, rgb[2] / w, w};
}

[[nodiscard]] inline Vector3 rgbFromInvertedCmyk(const InvertedCmyk& v) noexcept
{
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || !std::isfinite(v[3])) {
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }
    return {v[0] * v[3], v[1] * v[3], v[2] * v[3]};
}

} // namespace chromaroute::detail
