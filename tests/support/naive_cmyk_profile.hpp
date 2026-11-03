#pragma once

// CMYK ICC profiles for the tests (Milestone 8b): output profiles whose tables
// hold the naive formula (S5) with XYZ as the connection space, so expected
// values are known.

#include "support/icc_writer.hpp"

#include "engine/color_math.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace chromaroute::test {

inline const Matrix3 scrgb_matrix = detail::rgbToXyzD50(detail::srgb_chromaticities);
inline const Matrix3 scrgb_inverse = detail::inverse(scrgb_matrix);

// lut16Type stores XYZ as uInt16 / 32768 (ICC.1, 10.10), so a table value in
// [0, 1] is XYZ times this, and one table step is 1/32768 of XYZ.
inline constexpr double xyz_to_table = 32768.0 / 65535.0;

// The naive formula (S5): inks (1 = full ink) to linear scRGB, and back with
// full black replacement, clamped to [0, 1] as a table stores inks.
inline Vector3 naiveRgb(double c, double m, double y, double k)
{
    return {(1 - c) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k)};
}

inline std::array<double, 4> naiveInks(const Vector3& rgb)
{
    const double w = std::max({rgb[0], rgb[1], rgb[2]});
    std::array<double, 4> inks{};
    if (w > 0) {
        inks = {1 - rgb[0] / w, 1 - rgb[1] / w, 1 - rgb[2] / w, 1 - w};
    } else {
        inks = {1 - rgb[0], 1 - rgb[1], 1 - rgb[2], 1};
    }
    for (double& v : inks) {
        v = std::clamp(v, 0.0, 1.0);
    }
    return inks;
}

// A CMYK output profile (version 2) holding the naive formula: A2B0 on a grid
// of `a2b`, B2A0 on a grid of `b2a` (or none). `swap` exchanges cyan and
// magenta in both tables, which makes a different profile. `black` lifts the
// profile's black to that fraction of white: linear RGB black + (1 - black) x
// the formula, as a printer's darkest ink is not perfect black.
inline IccWriter naiveCmykProfile(std::size_t a2b, std::size_t b2a, bool swap = false, double black = 0.0)
{
    IccWriter w(2, "CMYK", "XYZ ", "prtr");
    w.xyz("wtpt", 0.9642, 1.0, 0.8249);
    w.lut16("A2B0", 4, 3, a2b, [&](const std::vector<double>& in) {
        const double c = swap ? in[1] : in[0];
        const double m = swap ? in[0] : in[1];
        const Vector3 rgb = naiveRgb(c, m, in[2], in[3]);
        const Vector3 lifted{black + (1 - black) * rgb[0], black + (1 - black) * rgb[1], black + (1 - black) * rgb[2]};
        const Vector3 xyz = detail::multiply(scrgb_matrix, lifted);
        return std::vector<double>{xyz[0] * xyz_to_table, xyz[1] * xyz_to_table, xyz[2] * xyz_to_table};
    });
    if (b2a != 0) {
        w.lut16("B2A0", 3, 4, b2a, [&](const std::vector<double>& in) {
            const Vector3 xyz{in[0] / xyz_to_table, in[1] / xyz_to_table, in[2] / xyz_to_table};
            const Vector3 lifted = detail::multiply(scrgb_inverse, xyz);
            const Vector3 rgb{(lifted[0] - black) / (1 - black), (lifted[1] - black) / (1 - black),
                              (lifted[2] - black) / (1 - black)};
            std::array<double, 4> inks = naiveInks(rgb);
            if (swap) {
                std::swap(inks[0], inks[1]);
            }
            return std::vector<double>(inks.begin(), inks.end());
        });
    }
    return w;
}

// CIE Lab relative to the ICC D50 illuminant as lcms2 writes it (cmsD50_XYZ),
// and lut16Type's Lab encoding (ICC.1 version 2): L 0..100 as 0..65280, a and
// b -128..127.996 as 0..65535 (256 per unit), each as a fraction of 65535.
inline constexpr Vector3 lcms_d50{0.9642, 1.0, 0.8249};

inline Vector3 labFromXyz(const Vector3& xyz)
{
    const auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16) / 116; };
    const double fx = f(xyz[0] / lcms_d50[0]);
    const double fy = f(xyz[1] / lcms_d50[1]);
    const double fz = f(xyz[2] / lcms_d50[2]);
    return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

inline Vector3 xyzFromLab(const Vector3& lab)
{
    const double fy = (lab[0] + 16) / 116;
    const double fx = fy + lab[1] / 500;
    const double fz = fy - lab[2] / 200;
    const auto g = [](double f) { return f > 6.0 / 29.0 ? f * f * f : (116 * f - 16) * 27.0 / 24389.0; };
    return {g(fx) * lcms_d50[0], g(fy) * lcms_d50[1], g(fz) * lcms_d50[2]};
}

inline Vector3 lut16FromLab(const Vector3& lab)
{
    return {lab[0] * 652.80 / 65535.0, (lab[1] + 128) * 256 / 65535.0, (lab[2] + 128) * 256 / 65535.0};
}

inline Vector3 labFromLut16(const std::vector<double>& v)
{
    return {v[0] * 65535.0 / 652.80, v[1] * 65535.0 / 256 - 128, v[2] * 65535.0 / 256 - 128};
}

// The naive profile with a Lab connection space, as most CMYK profiles have.
inline IccWriter naiveCmykLabProfile(std::size_t a2b, std::size_t b2a)
{
    IccWriter w(2, "CMYK", "Lab ", "prtr");
    w.xyz("wtpt", 0.9642, 1.0, 0.8249);
    w.lut16("A2B0", 4, 3, a2b, [&](const std::vector<double>& in) {
        const Vector3 xyz = detail::multiply(scrgb_matrix, naiveRgb(in[0], in[1], in[2], in[3]));
        const Vector3 encoded = lut16FromLab(labFromXyz(xyz));
        return std::vector<double>{encoded[0], encoded[1], encoded[2]};
    });
    w.lut16("B2A0", 3, 4, b2a, [&](const std::vector<double>& in) {
        const std::array<double, 4> inks = naiveInks(detail::multiply(scrgb_inverse, xyzFromLab(labFromLut16(in))));
        return std::vector<double>(inks.begin(), inks.end());
    });
    return w;
}

} // namespace chromaroute::test
