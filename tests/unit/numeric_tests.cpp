#include "support/test_harness.hpp"

#include "data/srgb_oracle_data.h"
#include "engine/numeric.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

using namespace chromaroute::detail;

namespace {

constexpr double positive_inf = std::numeric_limits<double>::infinity();
constexpr double quiet_nan = std::numeric_limits<double>::quiet_NaN();

} // namespace

CR_TEST(encode_unorm8_specials_and_range)
{
    CR_CHECK(encodeUnorm<std::uint8_t>(quiet_nan) == 0);
    CR_CHECK(encodeUnorm<std::uint8_t>(-quiet_nan) == 0);
    CR_CHECK(encodeUnorm<std::uint8_t>(positive_inf) == 255);
    CR_CHECK(encodeUnorm<std::uint8_t>(-positive_inf) == 0);
    CR_CHECK(encodeUnorm<std::uint8_t>(0.0) == 0);
    CR_CHECK(encodeUnorm<std::uint8_t>(-0.0) == 0);
    CR_CHECK(encodeUnorm<std::uint8_t>(-1e-300) == 0);
    CR_CHECK(encodeUnorm<std::uint8_t>(1.0) == 255);
    CR_CHECK(encodeUnorm<std::uint8_t>(1e300) == 255);
    CR_CHECK(encodeUnorm<std::uint8_t>(std::nextafter(1.0, 0.0)) == 255);
}

// For binary32 inputs, x * 255 + 0.5 is exact in double, so encode_unorm8
// must agree with the exact rule at every threshold the oracle lists.
CR_TEST(encode_unorm8_is_exact_at_float_thresholds)
{
    for (int k = 0; k < 255; ++k) {
        const float first_above = std::bit_cast<float>(chromaroute_oracle_f32_to_u8_threshold[k]);
        const float last_below = std::nextafter(first_above, 0.0f);
        const int above = encodeUnorm<std::uint8_t>(first_above);
        const int below = encodeUnorm<std::uint8_t>(last_below);
        CR_CHECK(above == k + 1);
        CR_CHECK(below == k);
    }
}

CR_TEST(decode_unorm8_exact_endpoints)
{
    CR_CHECK(decodeUnorm<std::uint8_t>(0) == 0.0);
    CR_CHECK(decodeUnorm<std::uint8_t>(255) == 1.0);
    for (int v = 0; v < 256; ++v) {
        CR_CHECK(encodeUnorm<std::uint8_t>(decodeUnorm(static_cast<std::uint8_t>(v))) == v);
    }
}

// For binary32 inputs x, x * 65535 is exact in double, so the threshold for
// level k + 1 is exactly the smallest float with x * 131070 >= 2k + 1.
CR_TEST(encode_unorm16_is_exact_at_every_float_threshold)
{
    int mismatches = 0;
    for (int k = 0; k < 65535; ++k) {
        float first = static_cast<float>((k + 0.5) / 65535.0);
        while (static_cast<double>(first) * 131070.0 < 2.0 * k + 1) {
            first = std::nextafter(first, 2.0f);
        }
        while (static_cast<double>(std::nextafter(first, 0.0f)) * 131070.0 >= 2.0 * k + 1) {
            first = std::nextafter(first, 0.0f);
        }
        mismatches += encodeUnorm<std::uint16_t>(first) != k + 1;
        mismatches += encodeUnorm<std::uint16_t>(std::nextafter(first, 0.0f)) != k;
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(unorm16_round_trip_and_correct_float_rounding)
{
    int round_trip = 0, misrounded = 0;
    for (std::uint32_t v = 0; v <= 0xFFFF; ++v) {
        const double decoded = decodeUnorm(static_cast<std::uint16_t>(v));
        round_trip += encodeUnorm<std::uint16_t>(decoded) != v;
        // f is the float nearest to v/65535 iff v/65535 lies within half the gap
        // to f's neighbor on that side; scaled by 65535 every term is exact.
        const float f = narrowToFloat(decoded);
        const double scaled = static_cast<double>(f) * 65535.0;
        const float neighbor = scaled > v ? std::nextafter(f, 0.0f) : std::nextafter(f, 2.0f);
        const double half_gap = std::fabs(static_cast<double>(neighbor) - f) / 2.0;
        misrounded += std::fabs(scaled - v) > half_gap * 65535.0;
    }
    CR_CHECK(round_trip == 0);
    CR_CHECK(misrounded == 0);
}

CR_TEST(unorm32_endpoints_rounding_and_round_trip)
{
    constexpr std::uint32_t max = 0xFFFFFFFFu;
    CR_CHECK(decodeUnorm(max) == 1.0);
    CR_CHECK(encodeUnorm<std::uint32_t>(1.0) == max);
    CR_CHECK(encodeUnorm<std::uint32_t>(std::nextafter(1.0, 0.0)) == max);
    CR_CHECK(encodeUnorm<std::uint32_t>(0.5) == 0x80000000u); // 2147483647.5 rounds up
    CR_CHECK(encodeUnorm<std::uint32_t>(quiet_nan) == 0);
    CR_CHECK(encodeUnorm<std::uint32_t>(positive_inf) == max);
    int round_trip = 0;
    std::uint32_t v = 12345;
    for (int i = 0; i < 1000000; ++i) {
        v = v * 1664525u + 1013904223u; // deterministic sample
        round_trip += encodeUnorm<std::uint32_t>(decodeUnorm(v)) != v;
    }
    for (std::uint32_t edge : {0u, 1u, 2u, max - 1, max}) {
        round_trip += encodeUnorm<std::uint32_t>(decodeUnorm(edge)) != edge;
    }
    CR_CHECK(round_trip == 0);
}

CR_TEST(narrow_to_float_overflow_follows_ieee_rounding)
{
    constexpr double float_max = std::numeric_limits<float>::max();
    constexpr float finf = std::numeric_limits<float>::infinity();
    const double midpoint = 0x1.ffffffp127; // float_max + half an ulp
    CR_CHECK(narrowToFloat(float_max) == std::numeric_limits<float>::max());
    CR_CHECK(narrowToFloat(std::nextafter(midpoint, 0.0)) == std::numeric_limits<float>::max());
    CR_CHECK(narrowToFloat(midpoint) == finf); // tie goes to the even neighbor, infinity
    CR_CHECK(narrowToFloat(1e300) == finf);
    CR_CHECK(narrowToFloat(-midpoint) == -finf);
    CR_CHECK(narrowToFloat(std::nextafter(-midpoint, 0.0)) == std::numeric_limits<float>::lowest());
    CR_CHECK(narrowToFloat(positive_inf) == finf);
    CR_CHECK(narrowToFloat(-positive_inf) == -finf);
    CR_CHECK(std::isnan(narrowToFloat(quiet_nan)));
    CR_CHECK(std::bit_cast<std::uint32_t>(narrowToFloat(-0.0)) == 0x80000000u);
    CR_CHECK(narrowToFloat(0x1p-149) == 0x1p-149f); // smallest subnormal survives
}

CR_TEST(srgb_transfer_functions_at_reference_points)
{
    CR_CHECK(srgbToLinear(0.0) == 0.0);
    CR_CHECK(std::fabs(srgbToLinear(1.0) - 1.0) < 1e-15);
    CR_CHECK(srgbToLinear(0.04045) == 0.04045 / 12.92); // linear segment includes the threshold
    CR_CHECK(linearToSrgb(0.0031308) == 0.0031308 * 12.92);
    CR_CHECK(std::fabs(linearToSrgb(1.0) - 1.0) < 1e-15);
    for (int v = 0; v <= 255; ++v) {
        const double encoded = v / 255.0;
        CR_CHECK(std::fabs(linearToSrgb(srgbToLinear(encoded)) - encoded) < 1e-12);
    }
    CR_CHECK(std::isnan(srgbToLinear(quiet_nan)));
    CR_CHECK(std::isnan(linearToSrgb(quiet_nan)));
}
