#pragma once

// CIE colorimetry for the CIE models (docs/model_coverage.md; decisions S1, S2,
// A7). Between models the colors are XYZ, or, when a Lab or LCH side is
// involved, XYZ normalized to the ICC D50 white, t = (X / Xn, Y / Yn, Z / Zn)
// with Yn = 1: the white is (1, 1, 1) and a neutral gray of luminance Y is
// (Y, Y, Y) exactly. Formulas are CIE 15:2004 (ISO/CIE 11664-4 and -5),
// evaluated in double (N1). Divisions are guarded, so no input divides by zero.

#include "engine/color_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace chromaroute::detail {

// The reference white of every CIE model: the ICC D50 PCS illuminant (N4).
inline constexpr Vector3 cie_white = icc_d50_white;

// CIE 15:2004 constants in their exact rational form.
inline constexpr double lab_epsilon = 216.0 / 24389.0; // (6/29)^3
inline constexpr double lab_kappa = 24389.0 / 27.0;    // (29/3)^3

// Chromaticity of the white, used for black in xyY and u'v' (it has none).
inline constexpr double white_sum = cie_white[0] + cie_white[1] + cie_white[2];
inline constexpr double white_x = cie_white[0] / white_sum;
inline constexpr double white_y = cie_white[1] / white_sum;
inline constexpr double white_denominator = cie_white[0] + 15.0 * cie_white[1] + 3.0 * cie_white[2];
inline constexpr double white_u = 4.0 * cie_white[0] / white_denominator;
inline constexpr double white_v = 9.0 * cie_white[1] / white_denominator;

[[nodiscard]] inline Vector3 xyzFromNormalized(const Vector3& t) noexcept
{
    return {t[0] * cie_white[0], t[1], t[2] * cie_white[2]};
}

[[nodiscard]] inline Vector3 normalizedFromXyz(const Vector3& xyz) noexcept
{
    return {xyz[0] / cie_white[0], xyz[1], xyz[2] / cie_white[2]};
}

// f(t0) - f(t1) for the CIE 15:2004 f(t): cbrt(t) above epsilon, else
// (kappa t + 16) / 116. When a linear branch is involved, the offset 16/116 =
// 4/29 is taken out of both terms first, so dark colors do not cancel against it.
[[nodiscard]] inline double labFDifference(double t0, double t1) noexcept
{
    if (t0 > lab_epsilon && t1 > lab_epsilon) {
        return std::cbrt(t0) - std::cbrt(t1);
    }
    const auto offset_free = [](double t) { return t > lab_epsilon ? std::cbrt(t) - 4.0 / 29.0 : lab_kappa * t / 116.0; };
    return offset_free(t0) - offset_free(t1);
}

[[nodiscard]] inline Vector3 labFromNormalized(const Vector3& t) noexcept
{
    // L = 116 f(ty) - 16, which on the linear branch is kappa ty itself.
    const double lightness = t[1] > lab_epsilon ? 116.0 * std::cbrt(t[1]) - 16.0 : lab_kappa * t[1];
    return {lightness, 500.0 * labFDifference(t[0], t[1]), 200.0 * labFDifference(t[1], t[2])};
}

// t for f = fy + a / 500 (or fy - b / 200): f^3 above 6/29, else
// (116 f - 16) / kappa. For the linear branch the caller passes 116 f - 16 as
// L + 116 a / 500 (or L - 116 b / 200), which does not cancel near black.
[[nodiscard]] inline double labFInverse(double f, double linear_numerator) noexcept
{
    return f > 6.0 / 29.0 ? f * f * f : linear_numerator / lab_kappa;
}

[[nodiscard]] inline Vector3 normalizedFromLab(const Vector3& lab) noexcept
{
    const double fy = (lab[0] + 16.0) / 116.0;
    const double ty = lab[0] > 8.0 ? fy * fy * fy : lab[0] / lab_kappa; // kappa * epsilon = 8
    return {labFInverse(fy + lab[1] / 500.0, lab[0] + lab[1] * (116.0 / 500.0)), ty,
            labFInverse(fy - lab[2] / 200.0, lab[0] - lab[2] * (116.0 / 200.0))};
}

// Chroma and hue in degrees, in [0, 360). An achromatic color (chroma exactly
// 0) has hue 0; -0 becomes +0.
[[nodiscard]] inline Vector3 lchFromLab(const Vector3& lab) noexcept
{
    const double chroma = std::hypot(lab[1], lab[2]);
    double hue = 0.0;
    if (chroma != 0.0) {
        hue = std::atan2(lab[2], lab[1]) * (180.0 / std::numbers::pi);
        if (hue < 0.0) {
            hue += 360.0;
        }
        if (hue >= 360.0) {
            hue -= 360.0; // a tiny negative angle plus 360 rounds to 360
        }
        hue += 0.0; // -0 -> +0
    }
    return {lab[0], chroma, hue};
}

// cos and sin of an angle in degrees. The angle is first reduced exactly: fmod
// to one turn, then r = turn - 90 q with q the nearest quadrant (|r| <= 45, and
// the subtraction is exact). Multiples of 90 therefore give exact zeros, and
// equal angles give equal results however many turns they include.
[[nodiscard]] inline std::array<double, 2> cosSinDegrees(double degrees) noexcept
{
    if (!std::isfinite(degrees)) {
        const double undefined = degrees - degrees; // NaN
        return {undefined, undefined};
    }
    const double turn = std::fmod(degrees, 360.0);
    const double quadrant = std::nearbyint(turn / 90.0);
    const double radians = (turn - 90.0 * quadrant) * (std::numbers::pi / 180.0);
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    switch ((static_cast<int>(quadrant) % 4 + 4) % 4) {
        case 0: return {c, s};
        case 1: return {-s, c};
        case 2: return {-c, -s};
        default: return {s, -c};
    }
}

[[nodiscard]] inline Vector3 labFromLch(const Vector3& lch) noexcept
{
    const auto [cosine, sine] = cosSinDegrees(lch[2]);
    return {lch[0], lch[1] * cosine + 0.0, lch[1] * sine + 0.0}; // + 0.0: -0 -> +0
}

// Black (X = Y = Z = 0, signed zeros too) has no chromaticity of its own.
[[nodiscard]] inline bool isBlack(const Vector3& xyz) noexcept
{
    return xyz[0] == 0.0 && xyz[1] == 0.0 && xyz[2] == 0.0;
}

// Chromaticities are ratios, so they are taken from XYZ scaled by a power of two
// that brings the largest component into [0.5, 1): the sums below cannot
// overflow. The scaling is exact, except that a component more than 2^1022
// times smaller than the largest may lose bits as a subnormal; its share of
// every ratio is then below the precision of the result. Non-finite and zero
// colors are returned unchanged.
[[nodiscard]] inline Vector3 scaledForRatios(const Vector3& xyz) noexcept
{
    const double largest = std::max({std::fabs(xyz[0]), std::fabs(xyz[1]), std::fabs(xyz[2])});
    if (!std::isfinite(largest) || largest == 0.0) {
        return xyz;
    }
    int exponent = 0;
    (void)std::frexp(largest, &exponent);
    return {std::ldexp(xyz[0], -exponent), std::ldexp(xyz[1], -exponent), std::ldexp(xyz[2], -exponent)};
}

// a * b / c * factor (a constant of moderate size) without intermediate
// overflow or underflow: the mantissas and the factor are combined first and
// the binary exponents applied once at the end, so a result in the subnormal
// range is rounded there only once. Within the normal range this equals
// a * b / c * factor bit for bit. c must be nonzero.
[[nodiscard]] inline double productQuotient(double a, double b, double c, double factor = 1.0) noexcept
{
    int ea = 0, eb = 0, ec = 0;
    const double ma = std::frexp(a, &ea);
    const double mb = std::frexp(b, &eb);
    const double mc = std::frexp(c, &ec);
    return std::ldexp(ma * mb / mc * factor, ea + eb - ec);
}

// The power of two by which the terms of a numerator such as 1 - x - y are
// scaled down, together with its divisor, so that their sum cannot overflow; the
// quotient is unchanged. It is 0, and results stay bit for bit the same, unless
// a chromaticity exceeds 2^1000 in magnitude: below that a sum of these few
// terms stays finite. Small terms scaled into the subnormal range are then far
// below the precision of the sum.
[[nodiscard]] inline int numeratorExponent(double a, double b) noexcept
{
    const double largest = std::max(std::fabs(a), std::fabs(b));
    if (!(largest > 0x1p1000) || !std::isfinite(largest)) {
        return 0;
    }
    int exponent = 0;
    (void)std::frexp(largest, &exponent);
    return exponent;
}

// xyY from (true) XYZ. Black takes the white's chromaticity; another color with
// X + Y + Z = 0, or a color with a non-finite component, has none (NaN).
[[nodiscard]] inline Vector3 xyyFromXyz(const Vector3& xyz) noexcept
{
    const Vector3 s = scaledForRatios(xyz);
    const double sum = s[0] + s[1] + s[2];
    const double undefined = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(sum)) {
        return {undefined, undefined, xyz[1]};
    }
    if (sum == 0.0) {
        const bool black = isBlack(xyz);
        return {black ? white_x : undefined, black ? white_y : undefined, xyz[1]};
    }
    return {s[0] / sum, s[1] / sum, xyz[1]};
}

// XYZ from xyY. y = 0 is black when Y = 0 (a NaN x stays NaN) and undefined
// (NaN) otherwise.
[[nodiscard]] inline Vector3 xyzFromXyy(const Vector3& xyy) noexcept
{
    const double x = xyy[0], y = xyy[1], luminance = xyy[2];
    if (y == 0.0) {
        const double v = luminance == 0.0 && !std::isnan(x) ? 0.0 : std::numeric_limits<double>::quiet_NaN();
        return {v, luminance, v};
    }
    // Z = (1 - x - y) Y / y, with the terms and y scaled together for extreme x, y.
    const int e = numeratorExponent(x, y);
    const double numerator = std::ldexp(1.0, -e) - std::ldexp(x, -e) - std::ldexp(y, -e);
    return {productQuotient(x, luminance, y), luminance, productQuotient(numerator, luminance, std::ldexp(y, -e))};
}

// CIE 1976 Y u' v' from (true) XYZ. Black takes the white's u' v'; another
// color with X + 15 Y + 3 Z = 0, or a color with a non-finite component, has
// none (NaN).
[[nodiscard]] inline Vector3 yuvFromXyz(const Vector3& xyz) noexcept
{
    const Vector3 s = scaledForRatios(xyz);
    const double denominator = s[0] + 15.0 * s[1] + 3.0 * s[2];
    const double undefined = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(denominator)) {
        return {xyz[1], undefined, undefined};
    }
    if (denominator == 0.0) {
        const bool black = isBlack(xyz);
        return {xyz[1], black ? white_u : undefined, black ? white_v : undefined};
    }
    return {xyz[1], 4.0 * s[0] / denominator, 9.0 * s[1] / denominator};
}

// XYZ from Y u' v'. v' = 0 is black when Y = 0 (a NaN u' stays NaN) and
// undefined (NaN) otherwise.
[[nodiscard]] inline Vector3 xyzFromYuv(const Vector3& yuv) noexcept
{
    const double luminance = yuv[0], u = yuv[1], v = yuv[2];
    if (v == 0.0) {
        const double value = luminance == 0.0 && !std::isnan(u) ? 0.0 : std::numeric_limits<double>::quiet_NaN();
        return {value, luminance, value};
    }
    // X = 9 u' Y / (4 v') and Z = Y (12 - 3 u' - 20 v') / (4 v'), with the terms
    // of Z's numerator and v' scaled together for extreme u', v'.
    const int e = numeratorExponent(u, v);
    const double scaled_v = std::ldexp(v, -e);
    const double numerator = std::ldexp(12.0, -e) - 3.0 * std::ldexp(u, -e) - 20.0 * scaled_v;
    return {productQuotient(u, luminance, v, 2.25), luminance, productQuotient(numerator, luminance, scaled_v, 0.25)};
}

} // namespace chromaroute::detail
