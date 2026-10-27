#pragma once

// RGB -> CIE XYZ (D50) matrices from chromaticities. Matrices are adapted to
// the ICC PCS white with the Bradford transform (decision N4). constexpr, so
// built-in spaces are derived at compile time from their defining numbers.

#include <chromaroute/color_space.hpp>

#include <array>

namespace chromaroute::detail {

using chromaroute::Chromaticities;
using chromaroute::Matrix3;
using chromaroute::Vector3;

// IEC 61966-2-1 sRGB primaries and D65 white point.
inline constexpr Chromaticities srgb_chromaticities{
    0.3127, 0.3290, 0.64, 0.33, 0.30, 0.60, 0.15, 0.06
};

// ICC PCS illuminant (D50) exactly as the ICC profile header encodes it in
// s15Fixed16: 0x0000F6D6, 0x00010000, 0x0000D32D.
inline constexpr Vector3 icc_d50_white{
    63190.0 / 65536.0, 1.0, 54061.0 / 65536.0
};

// Bradford cone response matrix (Lam 1985), as used for ICC chromatic adaptation.
inline constexpr Matrix3 bradford{{
    {0.8951, 0.2664, -0.1614},
    {-0.7502, 1.7135, 0.0367},
    {0.0389, -0.0685, 1.0296}
}};

[[nodiscard]] constexpr Vector3 xyToXyz(double x, double y) noexcept
{
    return {x / y, 1.0, (1.0 - x - y) / y};
}

[[nodiscard]] constexpr Vector3 multiply(const Matrix3& m, const Vector3& v) noexcept
{
    Vector3 result{};
    for (int r = 0; r < 3; ++r) {
        result[r] = m[r][0] * v[0] +
                    m[r][1] * v[1] +
                    m[r][2] * v[2];
    }
    return result;
}

[[nodiscard]] constexpr Matrix3 multiply(const Matrix3& a, const Matrix3& b) noexcept
{
    Matrix3 result{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            result[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        }
    }
    return result;
}

[[nodiscard]] constexpr double determinant(const Matrix3& m) noexcept
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - 
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Precondition: determinant(m) != 0 (true for any valid set of primaries).
[[nodiscard]] constexpr Matrix3 inverse(const Matrix3& m) noexcept
{
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = determinant(m);
    return {{{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
             {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
             {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}}};
}

// Bradford adaptation from one white (XYZ) to another.
[[nodiscard]] constexpr Matrix3 chromaticAdaptation(const Vector3& from_white, const Vector3& to_white) noexcept
{
    const Vector3 from = multiply(bradford, from_white);
    const Vector3 to = multiply(bradford, to_white);
    const Matrix3 scale{{
        {to[0] / from[0], 0.0, 0.0},
        {0.0, to[1] / from[1], 0.0},
        {0.0, 0.0, to[2] / from[2]}
    }};
    return multiply(inverse(bradford), multiply(scale, bradford));
}

// The primaries as XYZ columns with Y = 1 (red, green, blue).
// Precondition: every y is nonzero.
[[nodiscard]] constexpr Matrix3 primariesMatrix(const Chromaticities& c) noexcept
{
    const Vector3 red = xyToXyz(c.red_x, c.red_y);
    const Vector3 green = xyToXyz(c.green_x, c.green_y);
    const Vector3 blue = xyToXyz(c.blue_x, c.blue_y);
    return {{
        {red[0], green[0], blue[0]},
        {red[1], green[1], blue[1]},
        {red[2], green[2], blue[2]}
    }};
}

// Linear RGB -> XYZ relative to the ICC D50 white. Preconditions: every y is
// nonzero, determinant(primariesMatrix(c)) != 0, and no component of
// bradford x white is 0 (the space constructors check all three).
[[nodiscard]] constexpr Matrix3 rgbToXyzD50(const Chromaticities& c) noexcept
{
    const Vector3 white = xyToXyz(c.white_x, c.white_y);
    const Matrix3 primaries = primariesMatrix(c);
    const Vector3 scale = multiply(inverse(primaries), white); // primaries sum to the white point
    Matrix3 to_xyz{};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            to_xyz[r][k] = primaries[r][k] * scale[k];
        }
    }
    return multiply(chromaticAdaptation(white, icc_d50_white), to_xyz);
}

} // namespace chromaroute::detail
