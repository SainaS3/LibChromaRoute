// R1: "R'G'B'A u8" <-> "RGBA float" against the independent high-precision
// oracle (tests/data/srgb_oracle_data.hpp). Integer outputs and correctly
// rounded float outputs must match exactly.

#include "support/test_harness.hpp"

#include "data/srgb_oracle_data.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace oracle = chromaroute::test::oracle;

namespace {

Converter decoder()
{
    return *Converter::create(*PixelFormat::fromName("R'G'B'A u8"), *PixelFormat::fromName("RGBA float"));
}

Converter encoder()
{
    return *Converter::create(*PixelFormat::fromName("RGBA float"), *PixelFormat::fromName("R'G'B'A u8"));
}

std::vector<std::uint8_t> encode(const std::vector<float>& rgba)
{
    std::vector<std::uint8_t> out(rgba.size());
    const auto status = encoder().process(std::as_bytes(std::span(rgba)), std::as_writable_bytes(std::span(out)),
                                          rgba.size() / 4);
    if (!status) {
        out.assign(out.size(), 0xEE); // poison: any comparison will fail visibly
    }
    return out;
}

// Exact expected u8 for a float, by counting thresholds at or below it.
int expectedFromThresholds(const std::array<std::uint32_t, 255>& thresholds, float x)
{
    if (!(x > 0.0f)) {
        return 0;
    }
    const auto at = std::upper_bound(thresholds.begin(), thresholds.end(), std::bit_cast<std::uint32_t>(x));
    return static_cast<int>(at - thresholds.begin());
}

} // namespace

CR_TEST(r1_decode_all_u8_values_match_oracle)
{
    // Each channel sees all 256 values, in different orders.
    std::vector<std::uint8_t> source(256 * 4);
    for (int i = 0; i < 256; ++i) {
        source[i * 4 + 0] = static_cast<std::uint8_t>(i);
        source[i * 4 + 1] = static_cast<std::uint8_t>(255 - i);
        source[i * 4 + 2] = static_cast<std::uint8_t>((i * 7) & 0xFF);
        source[i * 4 + 3] = static_cast<std::uint8_t>((i * 101 + 3) & 0xFF);
    }
    std::vector<float> out(256 * 4);
    CR_CHECK(decoder().process(std::as_bytes(std::span(source)), std::as_writable_bytes(std::span(out)), 256).has_value());

    int mismatches = 0;
    for (int i = 0; i < 256 * 4; ++i) {
        const bool alpha = i % 4 == 3;
        const std::uint32_t expected =
            alpha ? oracle::u8_to_f32[source[i]] : oracle::srgb_u8_to_linear_f32[source[i]];
        mismatches += std::bit_cast<std::uint32_t>(out[i]) != expected;
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(r1_encode_is_exact_on_both_sides_of_every_threshold)
{
    // Color thresholds go through the sRGB OETF; alpha thresholds are linear.
    std::vector<float> rgba;
    std::vector<int> expected_color, expected_alpha;
    for (int k = 0; k < 255; ++k) {
        const float color_at = std::bit_cast<float>(oracle::linear_f32_to_srgb_u8_threshold[k]);
        const float alpha_at = std::bit_cast<float>(oracle::f32_to_u8_threshold[k]);
        for (int side = 0; side < 2; ++side) {
            const float c = side == 0 ? color_at : std::nextafter(color_at, 0.0f);
            const float a = side == 0 ? alpha_at : std::nextafter(alpha_at, 0.0f);
            rgba.insert(rgba.end(), {c, c, c, a});
            expected_color.push_back(side == 0 ? k + 1 : k);
            expected_alpha.push_back(side == 0 ? k + 1 : k);
        }
    }
    const std::vector<std::uint8_t> out = encode(rgba);
    int mismatches = 0;
    for (std::size_t p = 0; p < expected_color.size(); ++p) {
        for (int c = 0; c < 3; ++c) {
            mismatches += out[p * 4 + c] != expected_color[p];
        }
        mismatches += out[p * 4 + 3] != expected_alpha[p];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(r1_encode_dense_sweep_matches_threshold_model)
{
    constexpr int samples = 400001;
    std::vector<float> rgba(samples * 4);
    for (int i = 0; i < samples; ++i) {
        const float x = -0.05f + 1.1f * static_cast<float>(i) / static_cast<float>(samples - 1);
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = x;
    }
    const std::vector<std::uint8_t> out = encode(rgba);
    int color_mismatches = 0, alpha_mismatches = 0;
    for (int i = 0; i < samples; ++i) {
        const float x = rgba[i * 4];
        color_mismatches += out[i * 4] != expectedFromThresholds(oracle::linear_f32_to_srgb_u8_threshold, x);
        alpha_mismatches += out[i * 4 + 3] != expectedFromThresholds(oracle::f32_to_u8_threshold, x);
    }
    CR_CHECK(color_mismatches == 0);
    CR_CHECK(alpha_mismatches == 0);
}

CR_TEST(r1_encode_special_values)
{
    constexpr float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 10> inputs = {nan, -nan, inf, -inf, 0.0f, -0.0f, -1.0f, 2.0f,
                                          std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min()};
    const std::array<int, 10> expected = {0, 0, 255, 0, 0, 0, 0, 255, 255, 0};
    std::vector<float> rgba;
    for (float x : inputs) {
        rgba.insert(rgba.end(), {x, x, x, x});
    }
    const std::vector<std::uint8_t> out = encode(rgba);
    int mismatches = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            mismatches += out[i * 4 + c] != expected[i];
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(r1_u8_round_trip_is_lossless)
{
    std::vector<std::uint8_t> source(256 * 4);
    for (int i = 0; i < 256 * 4; ++i) {
        source[i] = static_cast<std::uint8_t>(i / 4);
    }
    std::vector<float> linear(source.size());
    std::vector<std::uint8_t> back(source.size());
    CR_CHECK(decoder().process(std::as_bytes(std::span(source)), std::as_writable_bytes(std::span(linear)), 256).has_value());
    CR_CHECK(encoder().process(std::as_bytes(std::span(linear)), std::as_writable_bytes(std::span(back)), 256).has_value());
    CR_CHECK(back == source);
}
