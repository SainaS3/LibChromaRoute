// binary16 codec: exhaustive round trips and every rounding midpoint.

#include "support/test_harness.hpp"

#include "engine/half.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

using namespace chromaroute::detail;

CR_TEST(half_decode_encode_round_trips_every_pattern)
{
    int mismatches = 0;
    for (std::uint32_t h = 0; h <= 0xFFFF; ++h) {
        const auto half = static_cast<std::uint16_t>(h);
        const double value = decodeHalf(half);
        const std::uint16_t back = encodeHalf(value);
        const bool is_nan = (half & 0x7C00) == 0x7C00 && (half & 0x03FF) != 0;
        if (is_nan) {
            mismatches += !std::isnan(value) || (back & 0x7C00) != 0x7C00 || (back & 0x03FF) == 0 ||
                          (back & 0x8000) != (half & 0x8000);
        } else {
            mismatches += back != half; // includes -0, subnormals, ±Inf
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(half_decode_known_values)
{
    CR_CHECK(decodeHalf(0x3C00) == 1.0);
    CR_CHECK(decodeHalf(0xC000) == -2.0);
    CR_CHECK(decodeHalf(0x7BFF) == 65504.0);
    CR_CHECK(decodeHalf(0x0400) == 0x1p-14);  // smallest normal
    CR_CHECK(decodeHalf(0x0001) == 0x1p-24);  // smallest subnormal
    CR_CHECK(decodeHalf(0x03FF) == 0x3FFp-24); // largest subnormal
    CR_CHECK(decodeHalf(0x7C00) == std::numeric_limits<double>::infinity());
    CR_CHECK(std::bit_cast<std::uint64_t>(decodeHalf(0x8000)) == 0x8000000000000000ull);
}

// For each pair of adjacent finite magnitudes, the midpoint is exact in double:
// it must round to the even neighbor, and one double ulp either side must
// round to the nearer neighbor.
CR_TEST(half_encode_rounds_to_nearest_even_at_every_midpoint)
{
    int mismatches = 0;
    for (std::uint32_t h = 0; h < 0x7BFF; ++h) {
        const auto lo = static_cast<std::uint16_t>(h);
        const auto hi = static_cast<std::uint16_t>(h + 1);
        const double mid = (decodeHalf(lo) + decodeHalf(hi)) / 2;
        const std::uint16_t even = (lo & 1) == 0 ? lo : hi;
        for (double sign : {1.0, -1.0}) {
            const std::uint16_t sign_bit = sign < 0 ? 0x8000 : 0;
            mismatches += encodeHalf(sign * mid) != (even | sign_bit);
            mismatches += encodeHalf(sign * std::nextafter(mid, 0.0)) != (lo | sign_bit);
            mismatches += encodeHalf(sign * std::nextafter(mid, 1e300)) != (hi | sign_bit);
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(half_encode_overflow_underflow_and_specials)
{
    // 65520 is the midpoint between 65504 (odd mantissa) and 2^16: ties go to infinity.
    CR_CHECK(encodeHalf(65520.0) == 0x7C00);
    CR_CHECK(encodeHalf(std::nextafter(65520.0, 0.0)) == 0x7BFF);
    CR_CHECK(encodeHalf(-65520.0) == 0xFC00);
    CR_CHECK(encodeHalf(1e300) == 0x7C00);
    CR_CHECK(encodeHalf(std::numeric_limits<double>::infinity()) == 0x7C00);
    CR_CHECK(encodeHalf(-std::numeric_limits<double>::infinity()) == 0xFC00);
    // Half the smallest subnormal ties to zero (even); anything above rounds up.
    CR_CHECK(encodeHalf(0x1p-25) == 0x0000);
    CR_CHECK(encodeHalf(std::nextafter(0x1p-25, 1.0)) == 0x0001);
    CR_CHECK(encodeHalf(0x1p-26) == 0x0000);
    CR_CHECK(encodeHalf(-0x1p-30) == 0x8000);
    CR_CHECK(encodeHalf(std::numeric_limits<double>::denorm_min()) == 0x0000);
    CR_CHECK(encodeHalf(-0.0) == 0x8000);
    // The largest subnormal plus half an ulp carries into the smallest normal.
    CR_CHECK(encodeHalf(0x3FFp-24 + 0x1p-25) == 0x0400);
    const std::uint16_t nan = encodeHalf(std::numeric_limits<double>::quiet_NaN());
    CR_CHECK((nan & 0x7C00) == 0x7C00 && (nan & 0x03FF) != 0);
}
