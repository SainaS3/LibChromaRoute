// HSL, HSV and HCY (Milestone 6): conversions against the oracle
// (tests/data/hue_oracle_data.hpp), exact primaries and neutrals, the hue
// rules, hue wrapping, undefined saturation, HCY's normalized luma weights
// (N12), copies within a model, the catalogue layouts and extreme values.

#include "support/conversion_checks.hpp"
#include "support/test_harness.hpp"

#include "core/descriptors.hpp"
#include "data/hue_oracle_data.hpp"

#include <chromaroute/chromaroute.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using chromaroute::ColorFamily;
using chromaroute::ColorSpace;
using chromaroute::Encoding;
using chromaroute::PixelFormat;
using chromaroute::TransferFunction;

namespace oracle = chromaroute::test::oracle;
using namespace chromaroute::test;

namespace {

constexpr double nan_value = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

bool sameBits(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

std::string in(const char* format, const ColorSpace& space)
{
    return std::string(format) + "-" + std::string(space.name());
}

// The model's double format, in sRGB unless a space suffix is given.
std::string doubleOf(const char* model, const char* space = "")
{
    return std::string(model) + " double" + space;
}

const char* const models[] = {"HSL", "HSV", "HCY"};

} // namespace

CR_TEST(rgb_to_hsl_hsv_hcy_matches_oracle)
{
    std::FILE* log = cr_context.out();
    const auto srgb = unpackRgb(oracle::hue_srgb_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "HSL float", srgb), oracle::hue_srgb_u8_to_hsl_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "HSV float", srgb), oracle::hue_srgb_u8_to_hsv_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "HCY float", srgb), oracle::hue_srgb_u8_to_hcy_f32, log) == 0);

    // ProPhoto: the sRGB curve on ProPhoto's linear RGB (S4), HCY with ProPhoto's
    // luma weights.
    const auto pro = unpackRgb(oracle::hue_prophoto_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "HSL float-ProPhoto", pro),
                           oracle::hue_prophoto_u8_to_hsl_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "HSV float-ProPhoto", pro),
                           oracle::hue_prophoto_u8_to_hsv_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "HCY float-ProPhoto", pro),
                           oracle::hue_prophoto_u8_to_hcy_f32, log) == 0);

    // Linear RGB float, including values outside [0, 1].
    const auto linear = floats(oracle::hue_linear_f32_inputs);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "HSV float", linear), oracle::hue_linear_f32_to_hsv_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "HCY float", linear), oracle::hue_linear_f32_to_hcy_f32, log) == 0);
}

CR_TEST(hsl_hsv_hcy_to_rgb_matches_oracle)
{
    std::FILE* log = cr_context.out();
    const auto hsl = floats(oracle::hue_hsl_f32_inputs);
    const auto hsv = floats(oracle::hue_hsv_f32_inputs);
    const auto hcy = floats(oracle::hue_hcy_f32_inputs);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("HSL float", "R'G'B' u8", hsl), oracle::hue_hsl_f32_to_srgb_u8, log) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("HSV float", "R'G'B' u8", hsv), oracle::hue_hsv_f32_to_srgb_u8, log) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("HCY float", "R'G'B' u8", hcy), oracle::hue_hcy_f32_to_srgb_u8, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("HSL float", "R'G'B' float", hsl), oracle::hue_hsl_f32_to_srgb_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("HSV float", "R'G'B' float", hsv), oracle::hue_hsv_f32_to_srgb_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("HCY float", "R'G'B' float", hcy), oracle::hue_hcy_f32_to_srgb_f32, log) == 0);
}

CR_TEST(primaries_and_secondaries_are_exact)
{
    // Hue in turns: red 0, yellow 1/6, green 1/3, cyan 1/2, blue 2/3, magenta 5/6.
    const std::vector<double> colors{1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    const double hues[] = {0.0, 1.0 / 6.0, 1.0 / 3.0, 0.5, 2.0 / 3.0, 5.0 / 6.0};
    const auto hsl = convert<double>("R'G'B' double", "HSL double", colors);
    const auto hsv = convert<double>("R'G'B' double", "HSV double", colors);
    const auto hcy = convert<double>("R'G'B' double", "HCY double", colors);
    const auto& y_row = PixelFormat::fromName("RGB double")->space().descriptor().luminance();
    for (std::size_t p = 0; p < 6; ++p) {
        CR_CHECK(hsl.size() == 18 && hsl[p * 3] == hues[p] && hsl[p * 3 + 1] == 1.0 && hsl[p * 3 + 2] == 0.5);
        CR_CHECK(hsv.size() == 18 && hsv[p * 3] == hues[p] && hsv[p * 3 + 1] == 1.0 && hsv[p * 3 + 2] == 1.0);
        CR_CHECK(hcy.size() == 18 && hcy[p * 3] == hues[p] && hcy[p * 3 + 1] == 1.0);
    }
    // HCY luma is the space's luminance of the encoded values: red's is its weight.
    CR_CHECK(hcy.size() == 18 && hcy[2] == y_row[0]);
    // And back, exactly.
    for (const char* model : models) {
        const auto back = convert<double>(doubleOf(model).c_str(), "R'G'B' double",
                                          convert<double>("R'G'B' double", doubleOf(model).c_str(), colors));
        CR_CHECK(sameBits(back, colors));
    }
}

CR_TEST(achromatic_colors_have_hue_zero_and_round_trip_exactly)
{
    // R' = G' = B' gives hue +0, saturation 0 and the value itself as
    // lightness, value and luma, also outside [0, 1]; saturation 0 converts back
    // to R' = G' = B' exactly, whatever the hue.
    std::vector<double> grays;
    for (double v : {0.0, 1e-300, 0.25, 0.5, 0.7, 1.0, -0.3, 1.8, 3e300}) {
        grays.insert(grays.end(), {v, v, v});
    }
    for (const char* model : models) {
        const auto values = convert<double>("R'G'B' double", doubleOf(model).c_str(), grays);
        bool ok = values.size() == grays.size();
        for (std::size_t p = 0; ok && p < grays.size() / 3; ++p) {
            ok = std::bit_cast<std::uint64_t>(values[p * 3]) == 0 && values[p * 3 + 1] == 0.0 &&
                 values[p * 3 + 2] == grays[p * 3];
        }
        CR_CHECK(ok);
        std::vector<double> zero_saturation;
        for (std::size_t p = 0; p < grays.size() / 3; ++p) {
            zero_saturation.insert(zero_saturation.end(), {0.37 * static_cast<double>(p), 0.0, grays[p * 3]});
        }
        CR_CHECK(sameBits(convert<double>(doubleOf(model).c_str(), "R'G'B' double", zero_saturation), grays));
    }
    // A hue computed as -0 is stored as +0.
    const auto signed_zero = convert<double>("R'G'B' double", "HSV double", std::vector<double>{1.0, -0.0, 0.0});
    CR_CHECK(signed_zero.size() == 3 && std::bit_cast<std::uint64_t>(signed_zero[0]) == 0);
}

CR_TEST(float_hue_models_store_the_hue_rules)
{
    // A hue that rounds to 1 in binary32 is stored as 0, directly and when
    // copied from double; double keeps it.
    const std::vector<double> near_full_turn{1.0, 0.0, 1e-9}; // hue 1 - 1e-9 / 6
    for (const char* model : models) {
        const std::string as_float = std::string(model) + " float";
        const std::string as_double = doubleOf(model);
        const auto in_double = convert<double>("R'G'B' double", as_double.c_str(), near_full_turn);
        const auto direct = convert<float>("R'G'B' double", as_float.c_str(), near_full_turn);
        const auto staged = convert<float>(as_double.c_str(), as_float.c_str(), in_double);
        CR_CHECK(in_double.size() == 3 && in_double[0] > 0.99 && in_double[0] < 1.0);
        CR_CHECK(direct.size() == 3 && std::bit_cast<std::uint32_t>(direct[0]) == 0);
        CR_CHECK(staged.size() == 3 && std::memcmp(direct.data(), staged.data(), 3 * sizeof(float)) == 0);
        // A computed saturation is at least about 2^-54, so only a stored value
        // can round to 0 in binary32: its hue is then stored as 0 too.
        const auto tiny = convert<float>(as_double.c_str(), as_float.c_str(), std::vector<double>{0.3, 1e-60, 0.5});
        CR_CHECK(tiny.size() == 3 && std::bit_cast<std::uint32_t>(tiny[0]) == 0 && tiny[1] == 0.0f && tiny[2] == 0.5f);
        const auto kept = convert<float>(as_double.c_str(), as_float.c_str(), std::vector<double>{0.3, 1e-30, 0.5});
        CR_CHECK(kept.size() == 3 && kept[0] == 0.3f);
        // In double, too, a hue that rounds to a full turn is 0: -1e-17 + 6
        // rounds to 6.
        const auto full_turn = convert<double>("R'G'B' double", as_double.c_str(), std::vector<double>{1.0, 0.0, 1e-17});
        CR_CHECK(full_turn.size() == 3 && std::bit_cast<std::uint64_t>(full_turn[0]) == 0 && full_turn[1] > 0.0);
    }
}

CR_TEST(hue_wraps_on_the_way_back)
{
    for (const char* model : models) {
        const std::string name = doubleOf(model);
        // A tiny negative hue is just below a full turn: red, not gray.
        const auto red = convert<double>(name.c_str(), "R'G'B' double", std::vector<double>{-1e-20, 1.0, 0.5});
        const auto red0 = convert<double>(name.c_str(), "R'G'B' double", std::vector<double>{0.0, 1.0, 0.5});
        CR_CHECK(sameBits(red, red0) && red.size() == 3 && red[0] > red[1] && red[1] == red[2]);
        // Hues outside [0, 1) are taken modulo one turn.
        const auto quarter = convert<double>(name.c_str(), "R'G'B' double", std::vector<double>{0.25, 0.8, 0.4});
        for (double hue : {1.25, -0.75, 7.25}) {
            CR_CHECK(sameBits(convert<double>(name.c_str(), "R'G'B' double", std::vector<double>{hue, 0.8, 0.4}), quarter));
        }
        // A NaN or infinite component gives NaN colors.
        for (const std::vector<double>& bad : {std::vector<double>{nan_value, 0.5, 0.5}, std::vector<double>{inf, 0.5, 0.5},
                                               std::vector<double>{0.2, nan_value, 0.5}, std::vector<double>{0.2, 0.0, nan_value},
                                               std::vector<double>{nan_value, 0.0, 0.5}}) {
            const auto rgb = convert<double>(name.c_str(), "R'G'B' double", bad);
            CR_CHECK(rgb.size() == 3 && std::isnan(rgb[0]) && std::isnan(rgb[1]) && std::isnan(rgb[2]));
        }
        const auto from_nan = convert<double>("R'G'B' double", name.c_str(), std::vector<double>{nan_value, 0.2, 0.3, inf, 0.0, 0.0});
        bool all_nan = from_nan.size() == 6;
        for (double v : from_nan) {
            all_nan = all_nan && std::isnan(v);
        }
        CR_CHECK(all_nan);
    }
}

CR_TEST(undefined_saturation_is_nan)
{
    // Outside the unit cube a color can have chroma where the relative
    // saturation has no value: HSL lightness 0, HSV value 0, HCY luma 0 or 1.
    const auto hsl = convert<double>("R'G'B' double", "HSL double", std::vector<double>{0.5, -0.5, 0.0});
    CR_CHECK(hsl.size() == 3 && hsl[2] == 0.0 && std::isnan(hsl[1]) && std::isfinite(hsl[0]));
    const auto hsv = convert<double>("R'G'B' double", "HSV double", std::vector<double>{0.0, -0.5, -0.2});
    CR_CHECK(hsv.size() == 3 && hsv[2] == 0.0 && std::isnan(hsv[1]) && std::isfinite(hsv[0]));
    // A matrix space with luma weights 1/4, 1/2, 1/4: (0.5, -0.25, 0) has luma 0.
    const chromaroute::Matrix3 m{{{0.5, 0.3, 0.2}, {0.25, 0.5, 0.25}, {0.1, 0.1, 0.8}}};
    const TransferFunction linear = TransferFunction::linear();
    const ColorSpace space = *ColorSpace::fromRgbToXyz("hcy-quarters", {1.0, 1.0, 1.0}, m, linear, linear, linear);
    const auto hcy = convert<double>(in("R~G~B~ double", space).c_str(), in("HCY double", space).c_str(),
                                     std::vector<double>{0.5, -0.25, 0.0, 1.5, 0.75, 1.0});
    CR_CHECK(hcy.size() == 6 && hcy[2] == 0.0 && std::isnan(hcy[1]) && hcy[5] == 1.0 && std::isnan(hcy[4]));
    // NaN saturation converts back as NaN.
    const auto back = convert<double>("HSL double", "R'G'B' double", hsl);
    CR_CHECK(back.size() == 3 && std::isnan(back[0]) && std::isnan(back[1]) && std::isnan(back[2]));
}

CR_TEST(colors_outside_the_unit_cube_round_trip)
{
    // Every model is invertible wherever saturation is defined. In double the
    // error, relative to the largest channel, is a few ulps: bound 2e-15 for HSL
    // and HSV (measured worst 1.2e-15 and 1.04e-15). HCY divides by the luma of
    // the hue's most chromatic color, which near blue is only about 0.06 in sRGB,
    // so the rounding of the hue grows by up to 1/0.06 on the way back: bound
    // 1.5e-14 (measured worst 7.35e-15).
    std::vector<double> colors;
    std::uint32_t state = 0x6A09E667u;
    for (int i = 0; i < 3 * 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        colors.push_back(-0.5 + 2.0 * (state >> 8) / 16777216.0);
    }
    for (const char* model : models) {
        const auto back = convert<double>(doubleOf(model).c_str(), "R'G'B' double",
                                          convert<double>("R'G'B' double", doubleOf(model).c_str(), colors));
        double worst = back.size() == colors.size() ? 0.0 : inf;
        for (std::size_t p = 0; worst != inf && p < colors.size() / 3; ++p) {
            const double size = std::fmax(std::fabs(colors[p * 3]), std::fmax(std::fabs(colors[p * 3 + 1]), std::fabs(colors[p * 3 + 2])));
            for (std::size_t c = 0; c < 3; ++c) {
                worst = worstOf(worst, std::fabs(back[p * 3 + c] - colors[p * 3 + c]) / size);
            }
        }
        std::fprintf(cr_context.out(), "  %s round trip: worst %.3g\n", model, worst);
        CR_CHECK(worst < (std::string(model) == "HCY" ? 1.5e-14 : 2e-15));
    }
}

CR_TEST(hcy_luma_weights_are_normalized_in_matrix_spaces)
{
    // N12: HCY's luma uses the space's luminance row divided by its sum, so a
    // gray value v (R = G = B = v, N11) has luma v whatever the matrix.
    const chromaroute::Matrix3 m{{{0.4360, 0.3851, 0.1431}, {0.2225, 0.7169, 0.0606}, {0.0139, 0.0971, 0.7141}}};
    chromaroute::Matrix3 twice = m;
    for (auto& row : twice) {
        for (double& v : row) {
            v *= 2.0;
        }
    }
    const TransferFunction srgb = TransferFunction::srgb();
    const ColorSpace one = *ColorSpace::fromRgbToXyz("hcy-sum-1", {0.9642, 1.0, 0.8251}, m, srgb, srgb, srgb);
    const ColorSpace two = *ColorSpace::fromRgbToXyz("hcy-sum-2", {1.9284, 2.0, 1.6502}, twice, srgb, srgb, srgb);
    const std::vector<double> colors{0.3, 0.3, 0.3, 0.9, 0.2, 0.4, 1.2, -0.1, 0.5};
    const auto hcy_one = convert<double>(in("R'G'B' double", one).c_str(), in("HCY double", one).c_str(), colors);
    const auto hcy_two = convert<double>(in("R'G'B' double", two).c_str(), in("HCY double", two).c_str(), colors);
    CR_CHECK(sameBits(hcy_one, hcy_two));
    CR_CHECK(hcy_two.size() == 9 && hcy_two[0] == 0.0 && hcy_two[1] == 0.0 && hcy_two[2] == 0.3);
    const auto back = convert<double>(in("HCY double", two).c_str(), in("R'G'B' double", two).c_str(), hcy_two);
    double worst = back.size() == colors.size() ? 0.0 : inf;
    for (std::size_t i = 0; worst != inf && i < colors.size(); ++i) {
        worst = worstOf(worst, std::fabs(back[i] - colors[i]));
    }
    CR_CHECK(worst < 4e-16); // a few ulps of values up to 1.2; measured worst 2.22e-16
    CR_CHECK(back.size() == 9 && back[0] == 0.3 && back[1] == 0.3 && back[2] == 0.3);

    // With a luminance row that sums to 0 the weights cannot be normalized: HCY
    // values are NaN in both directions, as gray values are (N11).
    const chromaroute::Matrix3 no_gray{{{1, 0, 0}, {1, -1, 0}, {0, 0, 1}}};
    const TransferFunction linear = TransferFunction::linear();
    const ColorSpace none = *ColorSpace::fromRgbToXyz("hcy-sum-0", {1, 1, 1}, no_gray, linear, linear, linear);
    const auto all_nan = [](const std::vector<double>& v, std::size_t count) {
        bool nan = v.size() == count;
        for (double x : v) {
            nan = nan && std::isnan(x);
        }
        return nan;
    };
    const auto undefined = convert<double>(in("R~G~B~ double", none).c_str(), in("HCY double", none).c_str(),
                                           std::vector<double>{0.4, 0.4, 0.4, 0.9, 0.1, 0.3});
    CR_CHECK(all_nan(undefined, 6)); // the gray too: every component
    const auto undefined_back = convert<double>(in("HCY double", none).c_str(), in("R~G~B~ double", none).c_str(),
                                                std::vector<double>{0.0, 0.0, 0.4, 0.3, 0.5, 0.4});
    CR_CHECK(all_nan(undefined_back, 6));
}

CR_TEST(hcy_chroma_is_undefined_where_luma_weights_make_it_singular)
{
    // With luma weights (1, 0, 0) every color of a hue whose largest and middle
    // channels are green and blue has peak luma 0, and every red-led hue has
    // peak luma 1. Below a peak of 0, or above a peak of 1, every chroma would
    // map to relative chroma 0, so it is undefined (NaN), not 0; HCY values with
    // chroma there have no color (NaN). Elsewhere in the space HCY round-trips.
    const chromaroute::Matrix3 m{{{0.5, 0.3, 0.2}, {1.0, 0.0, 0.0}, {0.0, 0.2, 0.8}}};
    const TransferFunction linear = TransferFunction::linear();
    const ColorSpace space = *ColorSpace::fromRgbToXyz("hcy-red-luma", {1.0, 1.0, 1.0}, m, linear, linear, linear);
    const std::string rgb = in("R~G~B~ double", space);
    const std::string hcy = in("HCY double", space);
    const auto values = convert<double>(rgb.c_str(), hcy.c_str(),
                                        std::vector<double>{-0.25, 0.5, -0.25, 1.5, 0.5, 0.2});
    // Peak 0, luma -0.25 below it; peak 1, luma 1.5 above it.
    CR_CHECK(values.size() == 6 && values[0] == 1.0 / 3.0 && std::isnan(values[1]) && values[2] == -0.25);
    CR_CHECK(values.size() == 6 && std::isfinite(values[3]) && std::isnan(values[4]) && values[5] == 1.5);
    for (const std::vector<double>& singular : {std::vector<double>{1.0 / 3.0, 0.5, -0.25}, std::vector<double>{0.05, 0.5, 1.5}}) {
        const auto back = convert<double>(hcy.c_str(), rgb.c_str(), singular);
        CR_CHECK(back.size() == 3 && std::isnan(back[0]) && std::isnan(back[1]) && std::isnan(back[2]));
    }
    // Controls: peak 1 with luma below it, and peak 0 with luma above it.
    const std::vector<double> regular{0.6, 0.2, 0.4, 0.2, 0.9, 0.5};
    const auto there = convert<double>(rgb.c_str(), hcy.c_str(), regular);
    const auto back = convert<double>(hcy.c_str(), rgb.c_str(), there);
    double worst = back.size() == regular.size() ? 0.0 : inf;
    for (std::size_t i = 0; worst != inf && i < regular.size(); ++i) {
        worst = worstOf(worst, std::fabs(back[i] - regular[i]));
    }
    CR_CHECK(there.size() == 6 && there[1] > 0.0 && there[4] > 0.0 && worst < 4e-16);

    // The singular points are decided with the plain weighted sum, where weights
    // of 0 add exactly 0. With the luminance row (0.1, 0, 0.5), normalized to
    // (1/6, 0, 5/6), HCY's luma form leaves -1.1e-16 for the green-led peak, so
    // without that the chroma would come out tiny but nonzero, and back huge.
    const chromaroute::Matrix3 no_green{{{0.4, 0.3, 0.2}, {0.1, 0.0, 0.5}, {0.0, 0.3, 0.6}}};
    const ColorSpace residue = *ColorSpace::fromRgbToXyz("hcy-no-green", {0.9, 0.6, 0.9}, no_green, linear, linear, linear);
    const std::string rgb2 = in("R~G~B~ double", residue);
    const std::string hcy2 = in("HCY double", residue);
    const auto green_led = convert<double>(rgb2.c_str(), hcy2.c_str(), std::vector<double>{-0.25, 0.5, -0.25});
    CR_CHECK(green_led.size() == 3 && green_led[0] == 1.0 / 3.0 && std::isnan(green_led[1]));
    const auto green_back = convert<double>(hcy2.c_str(), rgb2.c_str(), std::vector<double>{1.0 / 3.0, 0.5, -0.25});
    CR_CHECK(green_back.size() == 3 && std::isnan(green_back[0]) && std::isnan(green_back[1]) && std::isnan(green_back[2]));

    // Nonzero weights can cancel too, when one is negative: with the row
    // (1, -1, 1) cyan's peak is 1 - 1 = 0, so cyan colors below it are singular;
    // above it they round-trip.
    const chromaroute::Matrix3 cancel{{{0.5, 0.3, 0.2}, {1.0, -1.0, 1.0}, {0.0, 0.2, 0.8}}};
    const ColorSpace cancelling = *ColorSpace::fromRgbToXyz("hcy-cancel", {1.0, 1.0, 1.0}, cancel, linear, linear, linear);
    const std::string rgb3 = in("R~G~B~ double", cancelling);
    const std::string hcy3 = in("HCY double", cancelling);
    const auto cyan = convert<double>(rgb3.c_str(), hcy3.c_str(), std::vector<double>{-0.25, 0.5, 0.5, 0.2, 0.9, 0.9});
    CR_CHECK(cyan.size() == 6 && cyan[0] == 0.5 && std::isnan(cyan[1]) && cyan[2] == -0.25);
    CR_CHECK(cyan.size() == 6 && cyan[3] == 0.5 && cyan[4] > 0.0);
    const auto cyan_back = convert<double>(hcy3.c_str(), rgb3.c_str(), std::vector<double>{0.5, 0.5, -0.25});
    CR_CHECK(cyan_back.size() == 3 && std::isnan(cyan_back[0]) && std::isnan(cyan_back[1]) && std::isnan(cyan_back[2]));
    const auto cyan_round = convert<double>(hcy3.c_str(), rgb3.c_str(), std::vector<double>(cyan.begin() + 3, cyan.end()));
    CR_CHECK(cyan_round.size() == 3 && std::fabs(cyan_round[0] - 0.2) < 4e-16 && std::fabs(cyan_round[1] - 0.9) < 4e-16 &&
             std::fabs(cyan_round[2] - 0.9) < 4e-16);
}

CR_TEST(hue_models_follow_their_space_and_copy_within_a_model)
{
    // Between spaces with one matrix the values are copied, out-of-range hues
    // included; scRGB shares sRGB's primaries, and the ~ encoding ignores its
    // linear TRC.
    const std::vector<double> values{1.5, 0.4, 0.6, -0.2, 0.9, 0.3};
    for (const char* model : models) {
        CR_CHECK(sameBits(convert<double>(doubleOf(model).c_str(), doubleOf(model, "-scRGB").c_str(), values), values));
        // Into another space: equal to the route through R~G~B~ double.
        const auto direct = convert<double>(doubleOf(model).c_str(), doubleOf(model, "-Rec2020").c_str(), values);
        const auto staged = convert<double>("R~G~B~ double-Rec2020", doubleOf(model, "-Rec2020").c_str(),
                                            convert<double>(doubleOf(model).c_str(), "R~G~B~ double-Rec2020", values));
        CR_CHECK(sameBits(direct, staged) && !sameBits(direct, values));
    }
    // HSL and HSV of one color differ only where they should: same hue.
    const std::vector<double> color{0.8, 0.3, 0.1};
    const auto hsl = convert<double>("R'G'B' double", "HSL double", color);
    const auto hsv = convert<double>("R'G'B' double", "HSV double", color);
    const auto hcy = convert<double>("R'G'B' double", "HCY double", color);
    CR_CHECK(hsl.size() == 3 && hsv.size() == 3 && hcy.size() == 3 && hsl[0] == hsv[0] && hsv[0] == hcy[0]);
    CR_CHECK(std::fabs(hsl[2] - 0.45) < 1e-16 && hsv[2] == 0.8);
}

CR_TEST(hue_model_formats_have_their_catalogue_layouts)
{
    struct Expected {
        const char* name;
        ColorFamily family;
        std::size_t components, bytes;
        bool alpha;
    };
    const Expected cases[] = {
        {"HSL float", ColorFamily::hsl, 3, 12, false},   {"HSLA float", ColorFamily::hsl, 4, 16, true},
        {"HSL double", ColorFamily::hsl, 3, 24, false},  {"HSLA double", ColorFamily::hsl, 4, 32, true},
        {"HSV float", ColorFamily::hsv, 3, 12, false},   {"HSVA float", ColorFamily::hsv, 4, 16, true},
        {"HSV double", ColorFamily::hsv, 3, 24, false},  {"HSVA double", ColorFamily::hsv, 4, 32, true},
        {"HCY float", ColorFamily::hcy, 3, 12, false},   {"HCYA float", ColorFamily::hcy, 4, 16, true},
        {"HCY double", ColorFamily::hcy, 3, 24, false},  {"HCYA double", ColorFamily::hcy, 4, 32, true},
    };
    for (const Expected& e : cases) {
        for (const std::string& name : {std::string(e.name), std::string(e.name) + "-ProPhoto"}) {
            const auto f = PixelFormat::fromName(name);
            CR_CHECK(f.has_value());
            if (!f) {
                continue;
            }
            CR_CHECK(f->colorFamily() == e.family && f->encoding() == Encoding::perceptual);
            CR_CHECK(f->componentCount() == e.components && f->bytesPerPixel() == e.bytes);
            CR_CHECK(f->alpha() == (e.alpha ? chromaroute::Alpha::straight : chromaroute::Alpha::none));
        }
    }
    const auto& hcy = PixelFormat::fromName("HCYA float")->descriptor();
    CR_CHECK(hcy.model->components[0]->name == "hue" && hcy.model->components[1]->name == "HCY chroma" &&
             hcy.model->components[2]->name == "HCY luma" && hcy.model->components[3]->name == "A");
    const auto& hsv = PixelFormat::fromName("HSV double")->descriptor();
    CR_CHECK(hsv.model->components[1]->name == "saturation" && hsv.model->components[2]->name == "value");
    // Alpha is straight and passes through.
    const auto with_alpha = convert<double>("R'G'B'A double", "HSLA double", std::vector<double>{1.0, 0.0, 0.0, 0.25});
    CR_CHECK(with_alpha.size() == 4 && with_alpha[3] == 0.25 && with_alpha[1] == 1.0);
}

CR_TEST(hue_models_keep_finite_results_for_extreme_values)
{
    // HSV is homogeneous: scaling the color by 2^k scales the value and keeps
    // hue and saturation, bit for bit, also beyond 2^1000 where max - min
    // would overflow without the quarter-size evaluation.
    const std::vector<double> color{0.8, -0.6, 0.3};
    const auto base = convert<double>("R'G'B' double", "HSV double", color);
    int differ = base.size() != 3;
    for (int k = -900; k <= 1023 && differ == 0; ++k) {
        const std::vector<double> big{std::ldexp(color[0], k), std::ldexp(color[1], k), std::ldexp(color[2], k)};
        const auto hsv = convert<double>("R'G'B' double", "HSV double", big);
        differ += hsv.size() != 3 || bitsDiffer(hsv[0], base[0]) || bitsDiffer(hsv[1], base[1]) ||
                  bitsDiffer(hsv[2], std::ldexp(base[2], k));
        const auto back = convert<double>("HSV double", "R'G'B' double", hsv);
        const auto back_base = convert<double>("HSV double", "R'G'B' double", base);
        for (std::size_t c = 0; c < 3 && back.size() == 3 && back_base.size() == 3; ++c) {
            differ += bitsDiffer(back[c], std::ldexp(back_base[c], k));
        }
    }
    CR_CHECK(differ == 0);

    // HSL and HCY are not homogeneous; representable results stay finite where
    // a difference of channels would overflow, and convert back within a few
    // ulps of the largest value (bound 1e-15 of it, measured worst 2.35e-16).
    const std::vector<double> extremes{0x1.8p1023, -0x1p1022, 0.0, -1.2e308, 1.7e308, 1e308, 1e308, 1e308, 1e308};
    for (const char* model : models) {
        const auto values = convert<double>("R'G'B' double", doubleOf(model).c_str(), extremes);
        const auto back = convert<double>(doubleOf(model).c_str(), "R'G'B' double", values);
        bool ok = values.size() == extremes.size() && back.size() == extremes.size();
        double worst = ok ? 0.0 : inf;
        for (std::size_t i = 0; ok && i < extremes.size(); ++i) {
            ok = std::isfinite(values[i]) && std::isfinite(back[i]);
            worst = worstOf(worst, std::fabs(back[i] - extremes[i]) / 1.7e308);
        }
        CR_CHECK(ok && worst < 1e-15);
    }
    // 1.5 * 2^1023 and -2^1022: chroma 2^1024 would overflow, saturation -2 does not.
    const auto hsl = convert<double>("R'G'B' double", "HSL double", std::vector<double>{0x1.8p1023, -0x1p1022, 0.0});
    CR_CHECK(hsl.size() == 3 && hsl[0] == 23.0 / 24.0 && hsl[1] == -2.0 && hsl[2] == 0x1p1022);
}
