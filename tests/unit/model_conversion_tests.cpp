// Model conversions (gray <-> RGB, alpha add/drop) and TRCs across types,
// against the oracle (tests/data/pixel_format_oracle_data.hpp).

#include "support/test_harness.hpp"

#include "data/pixel_format_oracle_data.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace oracle = chromaroute::test::oracle;

namespace {

template <typename To, typename From>
std::vector<To> convert(const char* from, const char* to, const std::vector<From>& input)
{
    const Converter converter = *Converter::create(*PixelFormat::fromName(from), *PixelFormat::fromName(to));
    const std::size_t pixels = input.size() * sizeof(From) / converter.sourceFormat().bytesPerPixel();
    std::vector<To> output(pixels * converter.destinationFormat().bytesPerPixel() / sizeof(To));
    if (!converter.process(std::as_bytes(std::span(input)), std::as_writable_bytes(std::span(output)), pixels)) {
        output.assign(output.size(), To{}); // visible failure in any comparison
        output.push_back(To{});
    }
    return output;
}

} // namespace

CR_TEST(gray_u8_replicates_into_rgb_without_trc_round_trip)
{
    std::vector<std::uint8_t> gray(256);
    for (int v = 0; v < 256; ++v) {
        gray[v] = static_cast<std::uint8_t>(v);
    }
    const auto nonlinear = convert<std::uint8_t>("Y' u8", "R'G'B' u8", gray);
    const auto linear = convert<std::uint8_t>("Y u8", "RGB u8", gray);
    int mismatches = 0;
    for (int v = 0; v < 256; ++v) {
        for (int c = 0; c < 3; ++c) {
            mismatches += nonlinear[v * 3 + c] != v;
            mismatches += linear[v * 3 + c] != v;
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(gray_float_replicates_bit_exactly)
{
    const std::vector<float> gray = {0.0f, 0.25f, 1.0f, -0.5f, 3.0f, 0x1p-140f};
    const auto rgb = convert<float>("Y' float", "R'G'B' float", gray);
    int mismatches = 0;
    for (std::size_t i = 0; i < gray.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            mismatches += std::bit_cast<std::uint32_t>(rgb[i * 3 + c]) != std::bit_cast<std::uint32_t>(gray[i]);
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(rgb_u8_to_gray_u8_matches_oracle_luminance)
{
    constexpr std::size_t cases = std::size(oracle::gray_u8_inputs);
    std::vector<std::uint8_t> rgb(cases * 3);
    for (std::size_t i = 0; i < cases; ++i) {
        const std::uint32_t packed = oracle::gray_u8_inputs[i];
        rgb[i * 3 + 0] = static_cast<std::uint8_t>(packed);
        rgb[i * 3 + 1] = static_cast<std::uint8_t>(packed >> 8);
        rgb[i * 3 + 2] = static_cast<std::uint8_t>(packed >> 16);
    }
    const auto gray = convert<std::uint8_t>("R'G'B' u8", "Y' u8", rgb);
    int mismatches = 0;
    for (std::size_t i = 0; i < cases; ++i) {
        mismatches += gray[i] != oracle::gray_u8_expected[i];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(rgb_float_to_gray_float_matches_exact_weighted_sum)
{
    constexpr std::size_t cases = std::size(oracle::gray_f32_expected);
    std::vector<float> rgb(cases * 3);
    for (std::size_t i = 0; i < cases * 3; ++i) {
        rgb[i] = std::bit_cast<float>(oracle::gray_f32_inputs[i]);
    }
    const auto gray = convert<float>("RGB float", "Y float", rgb);
    int mismatches = 0;
    for (std::size_t i = 0; i < cases; ++i) {
        mismatches += std::bit_cast<std::uint32_t>(gray[i]) != oracle::gray_f32_expected[i];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(alpha_is_added_as_one_and_dropped_without_touching_color)
{
    const std::vector<std::uint8_t> rgb = {10, 20, 30, 200, 100, 0};
    const auto rgba = convert<std::uint8_t>("RGB u8", "RGBA u8", rgb);
    CR_CHECK((rgba == std::vector<std::uint8_t>{10, 20, 30, 255, 200, 100, 0, 255}));

    const auto half = convert<std::uint16_t>("R'G'B' float", "R'G'B'A half", std::vector<float>{0.5f, 0.25f, 1.0f});
    CR_CHECK(half.size() == 4 && half[3] == 0x3C00); // alpha 1.0 in binary16

    const auto dropped = convert<std::uint16_t>("RGBA u16", "RGB u16", std::vector<std::uint16_t>{1, 2, 3, 0, 4, 5, 6, 9});
    CR_CHECK((dropped == std::vector<std::uint16_t>{1, 2, 3, 4, 5, 6})); // straight alpha: colors unchanged

    const auto gray_alpha = convert<float>("Y'A u8", "RGBA float", std::vector<std::uint8_t>{255, 128});
    CR_CHECK(gray_alpha.size() == 4 && gray_alpha[0] == 1.0f && gray_alpha[3] == static_cast<float>(128 / 255.0));
}

CR_TEST(u16_decode_matches_oracle)
{
    constexpr std::size_t cases = std::size(oracle::u16_decode_inputs);
    std::vector<std::uint16_t> rgb(cases * 3);
    for (std::size_t i = 0; i < cases; ++i) {
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = oracle::u16_decode_inputs[i];
    }
    const auto linear = convert<float>("R'G'B' u16", "RGB float", rgb);
    int mismatches = 0;
    for (std::size_t i = 0; i < cases * 3; ++i) {
        mismatches += std::bit_cast<std::uint32_t>(linear[i]) != oracle::u16_decode_f32[i / 3];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(u16_encode_is_exact_at_sampled_thresholds)
{
    constexpr std::size_t cases = std::size(oracle::u16_encode_levels);
    std::vector<float> linear;
    for (std::size_t i = 0; i < cases; ++i) {
        const float at = std::bit_cast<float>(oracle::u16_encode_threshold[i]);
        linear.insert(linear.end(), {at, at, at, std::nextafter(at, 0.0f), std::nextafter(at, 0.0f), std::nextafter(at, 0.0f)});
    }
    const auto encoded = convert<std::uint16_t>("RGB float", "R'G'B' u16", linear);
    int mismatches = 0;
    for (std::size_t i = 0; i < cases; ++i) {
        const int k = oracle::u16_encode_levels[i];
        for (int c = 0; c < 3; ++c) {
            mismatches += encoded[i * 6 + c] != k + 1;
            mismatches += encoded[i * 6 + 3 + c] != k;
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(trc_round_trips_across_types)
{
    // Encoded -> linear -> encoded must be lossless for integer types.
    std::vector<std::uint16_t> u16(65536 * 3);
    for (std::size_t i = 0; i < u16.size(); ++i) {
        u16[i] = static_cast<std::uint16_t>(i / 3);
    }
    const auto through_float = convert<std::uint16_t>("RGB float", "R'G'B' u16", convert<float>("R'G'B' u16", "RGB float", u16));
    const auto through_double =
        convert<std::uint16_t>("RGB double", "R'G'B' u16", convert<double>("R'G'B' u16", "RGB double", u16));
    CR_CHECK(through_float == u16);
    CR_CHECK(through_double == u16);
}
