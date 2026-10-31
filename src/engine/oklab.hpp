#pragma once

// Oklab (decision S3): Ottosson's 2020 model with the matrices of CSS Color 4,
// which recalculate his for the D65 white x 0.3127, y 0.3290, so that D65 white
// is (1, 0, 0). The engine works with D50 XYZ, so the first matrix is applied
// after Bradford adaptation from the ICC D50 white to that D65: for column
// vectors, lms = M1 B XYZ_D50, then the cube root and M2. The inverses are
// computed from these matrices, as for color spaces (N4).

#include "engine/color_math.hpp"

#include <cmath>

namespace chromaroute::detail {

// CSS Color 4, "XYZ <-> LMS matrices recalculated for consistent reference white".
inline constexpr Matrix3 oklab_d65_xyz_to_lms{{
    {0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
    {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
    {0.0481771893596242, 0.2642395317527308, 0.6335478284694309},
}};

// CSS Color 4, "recalculated for 64bit precision".
inline constexpr Matrix3 oklab_lms_to_lab{{
    {0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
    {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
    {0.0259040424655478, 0.7827717124575296, -0.8086757549230774},
}};

inline constexpr Matrix3 oklab_xyz_to_lms =
    multiply(oklab_d65_xyz_to_lms, chromaticAdaptation(icc_d50_white, xyToXyz(0.3127, 0.3290)));
inline constexpr Matrix3 oklab_lms_to_xyz = inverse(oklab_xyz_to_lms);
inline constexpr Matrix3 oklab_lab_to_lms = inverse(oklab_lms_to_lab);

// Oklab is homogeneous of degree 1/3: XYZ scaled by 2^(3j) gives Oklab scaled
// by 2^j. XYZ beyond 2^1000, where M1 XYZ could overflow, or below 2^-900,
// where the cube roots would see subnormals, is therefore scaled by such a
// power of two first and the result scaled back; in between nothing changes.
[[nodiscard]] inline Vector3 oklabFromXyz(const Vector3& xyz) noexcept
{
    if (const int third = outOfRangeExponent(xyz, 0x1p-900, 0x1p1000) / 3; third != 0) {
        return scaled(oklabFromXyz(scaled(xyz, -3 * third)), third);
    }
    const Vector3 lms = multiply(oklab_xyz_to_lms, xyz);
    return multiply(oklab_lms_to_lab, Vector3{std::cbrt(lms[0]), std::cbrt(lms[1]), std::cbrt(lms[2])});
}

// The inverse; Oklab beyond 2^330 (whose cubes could overflow) or below 2^-300
// (whose cubes would be subnormal) is scaled first in the same way.
[[nodiscard]] inline Vector3 xyzFromOklab(const Vector3& lab) noexcept
{
    if (const int exponent = outOfRangeExponent(lab, 0x1p-300, 0x1p330); exponent != 0) {
        return scaled(xyzFromOklab(scaled(lab, -exponent)), 3 * exponent);
    }
    const Vector3 root = multiply(oklab_lab_to_lms, lab);
    return multiply(oklab_lms_to_xyz, Vector3{root[0] * root[0] * root[0], root[1] * root[1] * root[1],
                                              root[2] * root[2] * root[2]});
}

} // namespace chromaroute::detail
