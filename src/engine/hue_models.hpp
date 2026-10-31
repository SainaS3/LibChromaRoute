#pragma once

// HSL, HSV and HCY (decision S4): hue models computed from R'G'B' encoded with
// the sRGB curve in the format's space (the R~G~B~ values), with the reference
// catalogue's conventions. Hue is a fraction of a turn in [0, 1): 0 red, 1/3
// green, 2/3 blue. HSL's saturation, HSV's saturation and HCY's chroma are
// relative to the largest chroma at that hue and lightness, value or luma, so
// the unit cube maps to [0, 1] in each. HCY's luma uses the space's luminance
// weights, normalized to sum to 1 (decision N12).
//
// - An achromatic color (R' = G' = B') has hue +0 and saturation 0, and
//   saturation 0 converts back to R' = G' = B' exactly.
// - Hue wraps into [0, 1) on the way back, so -0.25 means 0.75.
// - A color outside the unit cube whose lightness is 0 or 1 (HSL), value 0
//   (HSV) or luma 0 or 1 (HCY), but whose chroma is not 0, has no saturation:
//   NaN. No division by zero.
// - HCY chroma is also undefined (NaN) where the luma weights make the hue's
//   most chromatic color have luma 0 with the color below it, or luma 1 with
//   the color above it: every chroma would map to 0 there (N12). That takes a
//   weight of 0, or weights that cancel because one is negative. An HCY value
//   with chroma at such a point has no color.
// - A NaN or infinite component, or NaN luma weights, give NaN in every color
//   component.

#include "engine/color_math.hpp"

#include <cmath>
#include <limits>

namespace chromaroute::detail {

// HCY's luma weights, normalized to sum to 1 (N12).
struct LumaWeights {
    double red;
    double green;
    double blue;
};

namespace hue_model {

inline constexpr double nan = std::numeric_limits<double>::quiet_NaN();
inline constexpr Vector3 nan_color{nan, nan, nan};

// Colors beyond 2^1000, where a difference such as high - low could overflow,
// are computed at a quarter of their size. The models are not homogeneous, so
// `unit` (1 or 1/4) then stands in for 1 in their constants.
inline constexpr double large = 0x1p1000;
inline constexpr double quarter = 0x1p-2;

[[nodiscard]] inline bool finite(const Vector3& v) noexcept
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

[[nodiscard]] inline double largest(const Vector3& v) noexcept
{
    return std::fmax(std::fabs(v[0]), std::fmax(std::fabs(v[1]), std::fabs(v[2])));
}

// Luma as G + Kr (R - G) + Kb (B - G): exactly R for R = G = B.
[[nodiscard]] inline double luma(const Vector3& rgb, const LumaWeights& w) noexcept
{
    return rgb[1] + w.red * (rgb[0] - rgb[1]) + w.blue * (rgb[2] - rgb[1]);
}

// Luma as the plain weighted sum, so channels of weight 0 add exactly 0: it
// decides whether a luma is exactly 0, as for HCY's singular points.
[[nodiscard]] inline double weightedSum(const Vector3& rgb, const LumaWeights& w) noexcept
{
    return w.red * rgb[0] + w.green * rgb[1] + w.blue * rgb[2];
}

[[nodiscard]] inline bool finite(const LumaWeights& w) noexcept
{
    return std::isfinite(w.red) && std::isfinite(w.green) && std::isfinite(w.blue);
}

// The hue of a color whose largest channel is `high`, with chroma > 0, in [0, 1).
[[nodiscard]] inline double hueOf(const Vector3& rgb, double high, double chroma) noexcept
{
    double sextant; // in [0, 6]
    if (high == rgb[0]) {
        sextant = (rgb[1] - rgb[2]) / chroma;
        if (sextant < 0.0) {
            sextant += 6.0;
        }
    } else if (high == rgb[1]) {
        sextant = 2.0 + (rgb[2] - rgb[0]) / chroma;
    } else {
        sextant = 4.0 + (rgb[0] - rgb[1]) / chroma;
    }
    const double hue = sextant / 6.0;
    return hue < 1.0 ? hue + 0.0 : 0.0; // a tiny negative sextant plus 6 rounds to 6; -0 -> +0
}

// The most chromatic color of a hue, wrapped into [0, 1) first: its largest
// channel 1, its smallest 0, the middle one rising or falling across each sixth
// of the turn.
[[nodiscard]] inline Vector3 hueColor(double hue) noexcept
{
    double turn = std::fmod(hue, 1.0); // exact
    if (turn < 0.0) {
        turn += 1.0; // a tiny negative hue plus 1 rounds to 1, which sector 5 maps to red
    }
    const double sextant = 6.0 * turn;
    const int sector = sextant < 5.0 ? static_cast<int>(sextant) : 5;
    const double f = sextant - sector;
    switch (sector) {
        case 0: return {1.0, f, 0.0};
        case 1: return {1.0 - f, 1.0, 0.0};
        case 2: return {0.0, 1.0, f};
        case 3: return {0.0, 1.0 - f, 1.0};
        case 4: return {f, 0.0, 1.0};
        default: return {1.0, 0.0, 1.0 - f};
    }
}

// The color of a hue (from hueColor) with the given smallest and largest
// channels and chroma between them, divided by `unit`. The largest and
// smallest channels are taken as they are, so primaries and secondaries come
// out exactly.
[[nodiscard]] inline Vector3 fromHueColor(const Vector3& color, double low, double high, double chroma,
                                          double unit) noexcept
{
    Vector3 rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double c = color[i] == 1.0 ? high : color[i] == 0.0 ? low : low + chroma * color[i];
        rgb[i] = c / unit;
    }
    return rgb;
}

// The scaled color and its unit for the forward conversions.
struct Scaled {
    Vector3 rgb;
    double unit;
    double high; // largest channel
    double low;  // smallest channel
};

[[nodiscard]] inline Scaled scaledForHue(const Vector3& input) noexcept
{
    const double unit = largest(input) > large ? quarter : 1.0;
    const Vector3 rgb = unit == 1.0 ? input : scaled(input, -2);
    return {rgb, unit, std::fmax(rgb[0], std::fmax(rgb[1], rgb[2])), std::fmin(rgb[0], std::fmin(rgb[1], rgb[2]))};
}

} // namespace hue_model

// HSL: lightness (high + low) / 2, saturation chroma / (1 - |2 lightness - 1|),
// with high and low the largest and smallest channels and chroma = high - low.
[[nodiscard]] inline Vector3 hslFromRgb(const Vector3& input) noexcept
{
    using namespace hue_model;
    if (!finite(input)) {
        return nan_color;
    }
    const auto [rgb, unit, high, low] = scaledForHue(input);
    const double chroma = high - low;
    const double sum = high + low;
    const double lightness = sum / 2.0 / unit;
    if (chroma == 0.0) {
        return {0.0, 0.0, lightness};
    }
    const double scale = sum <= unit ? sum : 2.0 * unit - sum; // unit (1 - |2 lightness - 1|)
    return {hueOf(rgb, high, chroma), scale != 0.0 ? chroma / scale : nan, lightness};
}

[[nodiscard]] inline Vector3 rgbFromHsl(const Vector3& hsl, double unit = 1.0) noexcept
{
    using namespace hue_model;
    if (!finite(hsl)) {
        return nan_color;
    }
    if (hsl[1] == 0.0) {
        return {hsl[2], hsl[2], hsl[2]};
    }
    const double lightness = hsl[2] * unit;
    const double twice = 2.0 * lightness;
    const double chroma = (twice <= unit ? twice : 2.0 * unit - twice) * hsl[1];
    if (unit == 1.0 && !(std::fabs(lightness) <= large && std::fabs(chroma) <= large)) {
        return rgbFromHsl(hsl, quarter);
    }
    const double half = chroma / 2.0;
    return fromHueColor(hueColor(hsl[0]), lightness - half, lightness + half, chroma, unit);
}

// HSV: value high, saturation chroma / value.
[[nodiscard]] inline Vector3 hsvFromRgb(const Vector3& input) noexcept
{
    using namespace hue_model;
    if (!finite(input)) {
        return nan_color;
    }
    const auto [rgb, unit, high, low] = scaledForHue(input);
    const double chroma = high - low;
    const double value = std::fmax(input[0], std::fmax(input[1], input[2]));
    if (chroma == 0.0) {
        return {0.0, 0.0, value};
    }
    return {hueOf(rgb, high, chroma), high != 0.0 ? chroma / high : nan, value};
}

[[nodiscard]] inline Vector3 rgbFromHsv(const Vector3& hsv, double unit = 1.0) noexcept
{
    using namespace hue_model;
    if (!finite(hsv)) {
        return nan_color;
    }
    if (hsv[1] == 0.0) {
        return {hsv[2], hsv[2], hsv[2]};
    }
    const double value = hsv[2] * unit;
    const double chroma = hsv[1] * value;
    if (unit == 1.0 && !(std::fabs(value) <= large && std::fabs(chroma) <= large)) {
        return rgbFromHsv(hsv, quarter);
    }
    return fromHueColor(hueColor(hsv[0]), value - chroma, value, chroma, unit);
}

// HCY: luma Y with the space's weights; chroma relative to that of the hue's
// most chromatic color, whose luma is the peak: chroma peak / Y below the peak,
// chroma (1 - peak) / (1 - Y) above it. The relative scale is singular where
// that factor, peak below it or 1 - peak above it, is 0. That takes a weight
// of 0 on the channels that carry the chroma, or nonzero weights that cancel
// because one is negative: with the row (1, -1, 1) cyan's peak is 0, and the
// built-in ACES2065-1 (blue weight -0.084) has hues whose peak is 0 or 1.
// Exactly 0 is decided with the plain weighted sum, where weights of 0 add
// exactly 0; a cancellation that leaves a rounding residue gives a tiny
// factor instead, and near such hues HCY is ill-conditioned.
[[nodiscard]] inline bool hcySingular(const Vector3& color, bool below, const LumaWeights& w) noexcept
{
    using hue_model::weightedSum;
    return below ? weightedSum(color, w) == 0.0 : weightedSum({1.0 - color[0], 1.0 - color[1], 1.0 - color[2]}, w) == 0.0;
}

[[nodiscard]] inline Vector3 hcyFromRgb(const Vector3& input, const LumaWeights& w) noexcept
{
    using namespace hue_model;
    if (!finite(input) || !finite(w)) {
        return nan_color; // NaN weights: a space whose luma weights cannot be normalized (N12)
    }
    const auto [rgb, unit, high, low] = scaledForHue(input);
    const double chroma = high - low;
    const double y = luma(rgb, w);
    if (chroma == 0.0) {
        return {0.0, 0.0, y / unit};
    }
    const Vector3 color{(rgb[0] - low) / chroma, (rgb[1] - low) / chroma, (rgb[2] - low) / chroma};
    const double peak = luma(color, w);
    const bool below = y < peak * unit;
    const double divisor = below ? y : unit - y;
    const double relative =
        divisor != 0.0 && !hcySingular(color, below, w) ? chroma * (below ? peak : 1.0 - peak) / divisor : nan;
    return {hueOf(rgb, high, chroma), relative, y / unit};
}

[[nodiscard]] inline Vector3 rgbFromHcy(const Vector3& hcy, const LumaWeights& w, double unit = 1.0) noexcept
{
    using namespace hue_model;
    if (!finite(hcy) || !finite(w)) {
        return nan_color; // NaN weights: a space whose luma weights cannot be normalized (N12)
    }
    if (hcy[1] == 0.0) {
        return {hcy[2], hcy[2], hcy[2]};
    }
    const Vector3 color = hueColor(hcy[0]);
    const double peak = luma(color, w);
    const double y = hcy[2] * unit;
    const bool below = y < peak * unit;
    if (hcySingular(color, below, w)) {
        return nan_color; // every color there has chroma 0
    }
    const double chroma = hcy[1] * (below ? y : unit - y) / (below ? peak : 1.0 - peak);
    if (unit == 1.0 && !(std::fabs(y) <= large && std::fabs(chroma) <= large)) {
        return rgbFromHcy(hcy, w, quarter);
    }
    const double low = y - chroma * peak;
    return fromHueColor(color, low, low + chroma, chroma, unit);
}

} // namespace chromaroute::detail
