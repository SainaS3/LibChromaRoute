// Associated alpha, gamma TRC thresholds and conversions across spaces,
// against the oracle (tests/data/color_semantics_oracle_data.hpp).

#include "support/test_harness.hpp"

#include "data/color_semantics_oracle_data.hpp"
#include "engine/numeric.hpp"
#include "engine/transfer.hpp"

#include <chromaroute/chromaroute.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
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
        output.push_back(To{}); // size mismatch makes any comparison fail
    }
    return output;
}

std::vector<std::uint8_t> unpackRgb(std::span<const std::uint32_t> packed)
{
    std::vector<std::uint8_t> rgb(packed.size() * 3);
    for (std::size_t i = 0; i < packed.size(); ++i) {
        rgb[i * 3 + 0] = static_cast<std::uint8_t>(packed[i]);
        rgb[i * 3 + 1] = static_cast<std::uint8_t>(packed[i] >> 8);
        rgb[i * 3 + 2] = static_cast<std::uint8_t>(packed[i] >> 16);
    }
    return rgb;
}

} // namespace

CR_TEST(alpha_floor_rule)
{
    using chromaroute::detail::alpha_floor;
    using chromaroute::detail::usedAlpha;
    CR_CHECK(usedAlpha(0.5) == 0.5 && usedAlpha(-0.5) == -0.5 && usedAlpha(1.5) == 1.5);
    CR_CHECK(usedAlpha(0.0) == alpha_floor && usedAlpha(-0.0) == alpha_floor);
    CR_CHECK(usedAlpha(alpha_floor) == alpha_floor);   // boundary included
    CR_CHECK(usedAlpha(-alpha_floor) == alpha_floor);  // positive floor even for tiny negatives
    CR_CHECK(usedAlpha(std::nextafter(alpha_floor, 1.0)) == std::nextafter(alpha_floor, 1.0));
    CR_CHECK(usedAlpha(-1e-7) == alpha_floor);
    CR_CHECK(std::isnan(usedAlpha(std::numeric_limits<double>::quiet_NaN())));
}

CR_TEST(associate_scales_color_by_used_alpha_and_keeps_alpha)
{
    const std::vector<float> rgba = {0.5f, 0.25f, 2.0f, 0.0f,   0.5f, 0.25f, 2.0f, 1e-7f, 0.5f, 0.25f, 2.0f, -0.25f,
                                     0.5f, 0.25f, 2.0f, 1.5f,   0.3f, 0.6f,  0.9f, 0.7f};
    const auto pre = convert<float>("RGBA float", "RaGaBaA float", rgba);
    int mismatches = 0;
    for (std::size_t p = 0; p < rgba.size() / 4; ++p) {
        const double a = rgba[p * 4 + 3];
        for (int c = 0; c < 3; ++c) {
            // A product of two binary32 values is exact in double; rounding it once is the definition.
            const float expected =
                chromaroute::detail::narrowToFloat(static_cast<double>(rgba[p * 4 + c]) * chromaroute::detail::usedAlpha(a));
            mismatches += std::bit_cast<std::uint32_t>(pre[p * 4 + c]) != std::bit_cast<std::uint32_t>(expected);
        }
        mismatches += std::bit_cast<std::uint32_t>(pre[p * 4 + 3]) != std::bit_cast<std::uint32_t>(rgba[p * 4 + 3]);
    }
    CR_CHECK(mismatches == 0);
    // Through double the round trip is exact, including alpha 0 (the floor is a power of two).
    const auto back = convert<float>("RaGaBaA double", "RGBA float", convert<double>("RGBA float", "RaGaBaA double", rgba));
    CR_CHECK(back == rgba);
}

CR_TEST(associated_u8_decode_matches_oracle)
{
    constexpr std::size_t cases = std::size(oracle::associated_u8_inputs);
    std::vector<std::uint8_t> pixels(cases * 4);
    for (std::size_t i = 0; i < cases; ++i) {
        const auto v = static_cast<std::uint8_t>(oracle::associated_u8_inputs[i]);
        const auto a = static_cast<std::uint8_t>(oracle::associated_u8_inputs[i] >> 8);
        pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = v;
        pixels[i * 4 + 3] = a;
    }
    const auto linear = convert<float>("R'aG'aB'aA u8", "RGBA float", pixels);
    int mismatches = 0;
    for (std::size_t i = 0; i < cases; ++i) {
        for (int c = 0; c < 3; ++c) {
            const bool bad = std::bit_cast<std::uint32_t>(linear[i * 4 + c]) != oracle::associated_u8_expected[i];
            if (bad && ++mismatches <= 5 && c == 0) {
                std::fprintf(cr_context.out(), "  v=%u a=%u: got %a expected %a\n", pixels[i * 4], pixels[i * 4 + 3],
                             static_cast<double>(linear[i * 4]),
                             static_cast<double>(std::bit_cast<float>(oracle::associated_u8_expected[i])));
            }
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(associated_formats_pass_through_when_domains_match)
{
    // Same domain on both sides: stored values are copied, never divided and re-multiplied.
    std::vector<std::uint16_t> u16(65536 * 4);
    for (std::size_t i = 0; i < u16.size(); ++i) {
        u16[i] = static_cast<std::uint16_t>((i * 7919) & 0xFFFF);
    }
    const auto through = convert<std::uint16_t>("R'aG'aB'aA double", "R'aG'aB'aA u16",
                                                convert<double>("R'aG'aB'aA u16", "R'aG'aB'aA double", u16));
    CR_CHECK(through == u16);
    // Luminance is linear, so it commutes with the scaling: YaA from RaGaBaA keeps alpha's scaling.
    const std::vector<double> pre = {0.1, 0.2, 0.3, 0.0, 0.4, 0.2, 0.1, 0.5};
    const auto gray = convert<double>("RaGaBaA double", "YaA double", pre);
    const auto& w = chromaroute::ColorSpace::srgb().luminance();
    CR_CHECK(gray[0] == w[0] * 0.1 + w[1] * 0.2 + w[2] * 0.3 && gray[1] == 0.0);
    CR_CHECK(gray[2] == w[0] * 0.4 + w[1] * 0.2 + w[2] * 0.1 && gray[3] == 0.5);
}

CR_TEST(kept_association_turns_nonfinite_alpha_into_nan)
{
    // Un-associating divides by a NaN or infinite alpha, so a model change gives
    // NaN colors even where the planner keeps the association (no TRC). A
    // type-only conversion copies.
    constexpr double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> pre = {0.5, 0.25, 1.0, nan, 0.5, 0.25, 1.0, inf, 0.5, 0.25, 1.0, -inf,
                                     0.5, 0.25, 1.0, 0.5, 0.5, 0.25, 1.0, 0.0};
    const auto gray = convert<double>("RaGaBaA double", "YaA double", pre);
    const auto rec = convert<double>("RaGaBaA double", "RaGaBaA double-Rec2020", pre);
    const auto via_straight = convert<double>("RGBA double", "YaA double", convert<double>("RaGaBaA double", "RGBA double", pre));
    const auto& w = chromaroute::ColorSpace::srgb().luminance();
    for (std::size_t p = 0; p < 3; ++p) {
        CR_CHECK(std::isnan(gray[p * 2]) && std::isnan(via_straight[p * 2]));
        CR_CHECK(std::isnan(rec[p * 4]) && std::isnan(rec[p * 4 + 1]) && std::isnan(rec[p * 4 + 2]));
    }
    CR_CHECK(gray[6] == w[0] * 0.5 + w[1] * 0.25 + w[2] * 1.0 && gray[7] == 0.5); // finite alpha: unchanged
    CR_CHECK(gray[8] == w[0] * 0.5 + w[1] * 0.25 + w[2] * 1.0 && gray[9] == 0.0); // alpha 0 too
    const auto copied = convert<float>("RaGaBaA double", "RaGaBaA float", pre);
    CR_CHECK(copied[0] == 0.5f && copied[4] == 0.5f && copied[8] == 0.5f && std::isnan(copied[3]));
}

CR_TEST(elided_gamma_trc_keeps_the_non_positive_clamp)
{
    // Same TRC on both sides, so decode + encode is skipped. A gamma decodes
    // values <= 0 to 0, and a model change must still give what that pair gives.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<double> gray = {-0.25, -0.0, 0.0, 0.5, 1.5, nan};
    const auto rgb = convert<double>("Y' double-Adobish", "R'G'B' double-Adobish", gray);
    const auto via_linear = convert<double>("RGB double-Adobish", "R'G'B' double-Adobish",
                                            convert<double>("Y' double-Adobish", "RGB double-Adobish", gray));
    const double expected[] = {0.0, 0.0, 0.0, 0.5, 1.5};
    int mismatches = 0;
    for (std::size_t p = 0; p < 5; ++p) {
        for (std::size_t c = 0; c < 3; ++c) {
            mismatches += std::bit_cast<std::uint64_t>(rgb[p * 3 + c]) != std::bit_cast<std::uint64_t>(expected[p]);
        }
        mismatches += (via_linear[p * 3] <= 0.0) != (expected[p] == 0.0);
    }
    CR_CHECK(mismatches == 0);
    CR_CHECK(std::isnan(rgb[15]) && std::isnan(rgb[16]) && std::isnan(rgb[17]));

    // Associated: the clamp applies to straight color, whose sign also depends
    // on alpha's (alpha -0.5 below), so the colors are un-associated first.
    const std::vector<double> pre = {-0.2, -0.5, 0.2, -0.5, -0.1, 0.5, 0.3, 0.5, -1e-6, 0.0, 3e-6, 0.0};
    const auto out = convert<float>("Y'aA double-Adobish", "R'aG'aB'aA float-Adobish", pre);
    const float want[][2] = {{static_cast<float>(-0.2), -0.5f}, {-0.0f, -0.5f}, {0.0f, 0.5f},
                             {static_cast<float>(0.3), 0.5f},   {0.0f, 0.0f},   {static_cast<float>(3e-6), 0.0f}};
    int wrong = 0;
    for (std::size_t p = 0; p < std::size(want); ++p) {
        for (std::size_t c = 0; c < 4; ++c) {
            wrong += std::bit_cast<std::uint32_t>(out[p * 4 + c]) != std::bit_cast<std::uint32_t>(want[p][c < 3 ? 0 : 1]);
        }
    }
    CR_CHECK(wrong == 0);

    // Same model and space: only the type changes, and values are kept.
    const auto widened = convert<double>("R'G'B' float-Adobish", "R'G'B' double-Adobish", std::vector<float>{-0.25f, 0.5f, -0.0f});
    CR_CHECK(widened == (std::vector<double>{-0.25, 0.5, -0.0}) && std::signbit(widened[2]));
}

CR_TEST(gamma22_encode_exact_at_every_threshold)
{
    std::vector<float> rgb;
    for (int k = 0; k < 255; ++k) {
        const float at = std::bit_cast<float>(oracle::gamma22_u8_threshold[k]);
        const float below = std::nextafter(at, 0.0f);
        rgb.insert(rgb.end(), {at, below, at});
    }
    const auto encoded = convert<std::uint8_t>("RGB float-Adobish", "R'G'B' u8-Adobish", rgb);
    int mismatches = 0;
    for (int k = 0; k < 255; ++k) {
        mismatches += encoded[k * 3] != k + 1;
        mismatches += encoded[k * 3 + 1] != k;
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(srgb_to_rec2020_linear_float_matches_exact_matrix)
{
    constexpr std::size_t values = std::size(oracle::srgb_to_rec2020_f32_inputs);
    std::vector<float> rgb(values);
    for (std::size_t i = 0; i < values; ++i) {
        rgb[i] = std::bit_cast<float>(oracle::srgb_to_rec2020_f32_inputs[i]);
    }
    const auto rec = convert<float>("RGB float", "RGB float-Rec2020", rgb);
    int mismatches = 0;
    for (std::size_t i = 0; i < values; ++i) {
        mismatches += std::bit_cast<std::uint32_t>(rec[i]) != oracle::srgb_to_rec2020_f32_expected[i];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(srgb_to_rec2020_encoded_u8_matches_oracle)
{
    constexpr std::size_t cases = std::size(oracle::srgb_to_rec2020_u8_inputs);
    const auto rec = convert<std::uint8_t>("R'G'B' u8", "R'G'B' u8-Rec2020",
                                           unpackRgb(oracle::srgb_to_rec2020_u8_inputs));
    int mismatches = 0;
    for (std::size_t i = 0; i < cases * 3; ++i) {
        mismatches += rec[i] != oracle::srgb_to_rec2020_u8_expected[i];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(gray_in_rec2020_uses_rec2020_luminance)
{
    constexpr std::size_t cases = std::size(oracle::rec2020_gray_u8_inputs);
    const auto gray = convert<std::uint8_t>("R'G'B' u8-Rec2020", "Y' u8-Rec2020",
                                            unpackRgb(oracle::rec2020_gray_u8_inputs));
    int mismatches = 0;
    for (std::size_t i = 0; i < cases; ++i) {
        mismatches += gray[i] != oracle::rec2020_gray_u8_expected[i];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(neutral_gray_and_round_trips_survive_space_changes)
{
    std::vector<std::uint8_t> gray(256 * 3);
    for (std::size_t i = 0; i < gray.size(); ++i) {
        gray[i] = static_cast<std::uint8_t>(i / 3);
    }
    // R = G = B stays R = G = B in every space; spaces that also use the sRGB
    // curve (Rec2020, DisplayP3) even keep the values.
    int not_neutral = 0, changed = 0;
    for (const char* to : {"R'G'B' u8-Rec2020", "R'G'B' u8-DisplayP3", "R'G'B' u8-ProPhoto", "R'G'B' u8-ACEScg"}) {
        const auto out = convert<std::uint8_t>("R'G'B' u8", to, gray);
        for (std::size_t p = 0; p < 256; ++p) {
            not_neutral += out[p * 3] != out[p * 3 + 1] || out[p * 3] != out[p * 3 + 2];
        }
        const bool srgb_curve = std::string(to).find("Rec2020") != std::string::npos ||
                                std::string(to).find("DisplayP3") != std::string::npos;
        changed += srgb_curve && out != gray;
    }
    CR_CHECK(not_neutral == 0);
    CR_CHECK(changed == 0);
    // sRGB float -> Rec2020 double -> sRGB float returns the original floats.
    std::vector<float> rgb;
    for (int i = 0; i < 3000; ++i) {
        rgb.push_back(static_cast<float>(-0.2 + 1.4 * ((i * 7919) % 3001) / 3000.0));
    }
    const auto back = convert<float>("RGB double-Rec2020", "RGB float", convert<double>("RGB float", "RGB double-Rec2020", rgb));
    CR_CHECK(back == rgb);
}
