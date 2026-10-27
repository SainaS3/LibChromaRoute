// sRGB -> XYZ (D50) derivation (decision N4) against the exact rational oracle.

#include "support/test_harness.hpp"

#include "core/descriptors.hpp"
#include "data/pixel_format_oracle_data.h"
#include "engine/color_math.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

using namespace chromaroute::detail;

namespace {

std::int64_t ulpDistance(double a, double b)
{
    const auto ordered = [](double v) {
        const auto bits = std::bit_cast<std::int64_t>(v);
        return bits >= 0 ? bits : std::int64_t(0x8000000000000000ull - static_cast<std::uint64_t>(bits));
    };
    const std::int64_t d = ordered(a) - ordered(b);
    return d < 0 ? -d : d;
}

} // namespace

// The compile-time derivation runs ~150 double operations (two 3x3 inversions,
// four products). Measured worst element: 14 ulp (MSVC 19.51, 2026-10-23);
// the bound allows that plus 2 ulp, and is far below anything visible.
CR_TEST(srgb_matrix_matches_exact_derivation)
{
    const Matrix3& m = srgbSpace().rgb_to_xyz;
    std::int64_t worst = 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const std::int64_t d = ulpDistance(m[r][c], chromaroute_oracle_srgb_to_xyz_d50[r * 3 + c]);
            worst = d > worst ? d : worst;
        }
    }
    std::fprintf(cr_context.out(), "  sRGB matrix: worst element %lld ulp from exact\n", static_cast<long long>(worst));
    CR_CHECK(worst <= 16);
}

CR_TEST(srgb_white_maps_to_d50_and_luminance_sums_to_one)
{
    const Matrix3& m = srgbSpace().rgb_to_xyz;
    const Vector3 white = multiply(m, Vector3{1.0, 1.0, 1.0});
    for (int i = 0; i < 3; ++i) {
        CR_CHECK(std::fabs(white[i] - icc_d50_white[i]) < 1e-15);
    }
    const Vector3& y = srgbSpace().luminance();
    CR_CHECK(std::fabs(y[0] + y[1] + y[2] - 1.0) < 1e-15);
    CR_CHECK(&y == &m[1]);
}

CR_TEST(inverse_and_adaptation_are_consistent)
{
    const Matrix3& m = srgbSpace().rgb_to_xyz;
    const Matrix3 identity = multiply(m, inverse(m));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CR_CHECK(std::fabs(identity[r][c] - (r == c ? 1.0 : 0.0)) < 1e-15);
        }
    }
    const Vector3 d65 = xyToXyz(0.3127, 0.3290);
    const Vector3 adapted = multiply(chromaticAdaptation(d65, icc_d50_white), d65);
    for (int i = 0; i < 3; ++i) {
        CR_CHECK(std::fabs(adapted[i] - icc_d50_white[i]) < 1e-15);
    }
}
