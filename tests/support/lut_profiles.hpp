#pragma once

// RGB and gray ICC profiles built on lookup tables, for the tests (Milestone
// 8c, decision A12). Their tables hold known formulas with XYZ (or Lab) as the
// connection space, so expected values are known: device values are linear
// sRGB (scRGB) through a curve, by default the identity, which makes both
// tables linear maps that lcms2's tetrahedral interpolation reproduces up to
// rounding.

#include "support/naive_cmyk_profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace chromaroute::test {

struct LutRgbOptions {
    std::size_t a2b = 18; // 17 cells: every node is a whole number of 16-bit steps
    std::size_t b2a = 52; // 51 cells: likewise for the XYZ nodes
    bool squared = false; // linear = device^2 instead of linear = device
    bool swap = false;    // red and green exchanged in both tables
    double black = 0.0;   // linear black lifted to this fraction of white
    bool lab = false;     // Lab connection space instead of XYZ
    const char* device_class = "prtr";
    bool relative = false; // the same tables as A2B1 and B2A1 too
};

// Device R'G'B' to linear scRGB, and back with the result clamped to [0, 1]
// as a table stores it.
inline Vector3 lutLinear(const LutRgbOptions& o, double r, double g, double b)
{
    Vector3 v = o.swap ? Vector3{g, r, b} : Vector3{r, g, b};
    for (double& x : v) {
        x = o.squared ? x * x : x;
        x = o.black + (1 - o.black) * x;
    }
    return v;
}

inline Vector3 lutDevice(const LutRgbOptions& o, const Vector3& linear)
{
    Vector3 v{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double x = std::clamp((linear[i] - o.black) / (1 - o.black), 0.0, 1.0);
        v[i] = o.squared ? std::sqrt(x) : x;
    }
    return o.swap ? Vector3{v[1], v[0], v[2]} : v;
}

inline std::vector<double> lutPcsValues(const LutRgbOptions& o, const Vector3& xyz)
{
    const Vector3 e = o.lab ? lut16FromLab(labFromXyz(xyz))
                            : Vector3{xyz[0] * xyz_to_table, xyz[1] * xyz_to_table, xyz[2] * xyz_to_table};
    return {e[0], e[1], e[2]};
}

inline Vector3 lutXyzOf(const LutRgbOptions& o, const std::vector<double>& in)
{
    return o.lab ? xyzFromLab(labFromLut16(in)) : Vector3{in[0] / xyz_to_table, in[1] / xyz_to_table, in[2] / xyz_to_table};
}

// An RGB profile (version 2) holding the formula in A2B0, and its inverse in
// B2A0 unless `encode` is false (an input-only profile).
inline IccWriter lutRgbProfile(const LutRgbOptions& o = {}, bool encode = true)
{
    IccWriter w(2, "RGB ", o.lab ? "Lab " : "XYZ ", o.device_class);
    w.xyz("wtpt", 0.9642, 1.0, 0.8249);
    for (const char* sig : o.relative ? std::vector<const char*>{"A2B0", "A2B1"} : std::vector<const char*>{"A2B0"}) {
        w.lut16(sig, 3, 3, o.a2b, [&](const std::vector<double>& in) {
            return lutPcsValues(o, detail::multiply(scrgb_matrix, lutLinear(o, in[0], in[1], in[2])));
        });
    }
    for (const char* sig : !encode       ? std::vector<const char*>{}
                           : o.relative ? std::vector<const char*>{"B2A0", "B2A1"}
                                        : std::vector<const char*>{"B2A0"}) {
        w.lut16(sig, 3, 3, o.b2a, [&](const std::vector<double>& in) {
            const Vector3 d = lutDevice(o, detail::multiply(scrgb_inverse, lutXyzOf(o, in)));
            return std::vector<double>{d[0], d[1], d[2]};
        });
    }
    return w;
}

// A gray profile (version 2): A2B0 maps a gray value y to y x `tint` (by
// default the D50 white; y^2 when squared), B2A0 back from XYZ's Y.
inline IccWriter lutGrayProfile(std::size_t a2b = 18, std::size_t b2a = 52, bool squared = false,
                                const char* device_class = "spac", Vector3 tint = lcms_d50)
{
    IccWriter w(2, "GRAY", "XYZ ", device_class);
    w.xyz("wtpt", 0.9642, 1.0, 0.8249);
    w.lut16("A2B0", 1, 3, a2b, [&](const std::vector<double>& in) {
        const double y = squared ? in[0] * in[0] : in[0];
        return std::vector<double>{y * tint[0] * xyz_to_table, y * tint[1] * xyz_to_table, y * tint[2] * xyz_to_table};
    });
    w.lut16("B2A0", 3, 1, b2a, [&](const std::vector<double>& in) {
        const double y = std::clamp(in[1] / xyz_to_table, 0.0, 1.0);
        return std::vector<double>{squared ? std::sqrt(y) : y};
    });
    return w;
}

} // namespace chromaroute::test
