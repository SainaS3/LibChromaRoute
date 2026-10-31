#pragma once

// Y'CbCr (decision S4): computed from R'G'B' encoded with the sRGB curve in the
// format's space (the R~G~B~ values), with BT.601 or BT.709 luma weights. Cb
// and Cr span -0.5..0.5. The coefficients are derived from the two weights, not
// rounded constants.

#include "engine/color_math.hpp"

namespace chromaroute::detail {

struct YcbcrCoefficients {
    double red;           // Kr
    double blue;          // Kb
    double blue_scale;    // B' - Y' = blue_scale Cb: 2 (1 - Kb)
    double red_scale;     // R' - Y' = red_scale Cr: 2 (1 - Kr)
    double green_from_cb; // G' = Y' - green_from_cb Cb - green_from_cr Cr
    double green_from_cr;
};

[[nodiscard]] constexpr YcbcrCoefficients ycbcrCoefficients(double red, double blue) noexcept
{
    const double green = 1.0 - red - blue;
    const double blue_scale = 2.0 * (1.0 - blue);
    const double red_scale = 2.0 * (1.0 - red);
    return {red, blue, blue_scale, red_scale, blue * blue_scale / green, red * red_scale / green};
}

inline constexpr YcbcrCoefficients bt601_ycbcr = ycbcrCoefficients(0.299, 0.114);
inline constexpr YcbcrCoefficients bt709_ycbcr = ycbcrCoefficients(0.2126, 0.0722);

// Y'CbCr is linear, so a power-of-two scale commutes with it exactly. Values
// beyond 2^1000, where a difference such as R' - G' or a product such as
// 1.402 Cr could overflow, are scaled down first and the result scaled back;
// below that nothing changes.

// Y' = Kr R' + Kg G' + Kb B', written as G' + Kr (R' - G') + Kb (B' - G') so that
// a neutral (R' = G' = B') gives Y' = G' and Cb = Cr = 0 exactly.
[[nodiscard]] inline Vector3 ycbcrFromRgb(const Vector3& rgb, const YcbcrCoefficients& k) noexcept
{
    if (const int e = outOfRangeExponent(rgb, 0.0, 0x1p1000); e != 0) {
        return scaled(ycbcrFromRgb(scaled(rgb, -e), k), e);
    }
    const double luma = rgb[1] + k.red * (rgb[0] - rgb[1]) + k.blue * (rgb[2] - rgb[1]);
    return {luma, (rgb[2] - luma) / k.blue_scale, (rgb[0] - luma) / k.red_scale};
}

// The inverse; Cb = Cr = 0 gives R' = G' = B' = Y' exactly.
[[nodiscard]] inline Vector3 rgbFromYcbcr(const Vector3& ycc, const YcbcrCoefficients& k) noexcept
{
    if (const int e = outOfRangeExponent(ycc, 0.0, 0x1p1000); e != 0) {
        return scaled(rgbFromYcbcr(scaled(ycc, -e), k), e);
    }
    return {ycc[0] + k.red_scale * ycc[2], ycc[0] - k.green_from_cb * ycc[1] - k.green_from_cr * ycc[2],
            ycc[0] + k.blue_scale * ycc[1]};
}

} // namespace chromaroute::detail
