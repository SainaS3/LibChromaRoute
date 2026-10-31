// Naive CMYK (Milestone 7): conversions against the oracle
// (tests/data/cmyk_oracle_data.hpp), the reference catalogue's conventions
// (S5), linear scRGB whatever the format's space, round trips, conversions
// among the CMYK models, neutrals, NaN, and the catalogue layouts.

#include "support/conversion_checks.hpp"
#include "support/test_harness.hpp"

#include "core/descriptors.hpp"
#include "data/cmyk_oracle_data.hpp"

#include <chromaroute/chromaroute.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using chromaroute::Alpha;
using chromaroute::ColorFamily;
using chromaroute::Encoding;
using chromaroute::PixelFormat;

namespace oracle = chromaroute::test::oracle;
using namespace chromaroute::test;

namespace {

constexpr double nan_value = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

bool sameBits(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

bool allNan(const std::vector<double>& v, std::size_t count)
{
    bool nan = v.size() == count;
    for (double x : v) {
        nan = nan && std::isnan(x);
    }
    return nan;
}

} // namespace

CR_TEST(rgb_to_cmyk_matches_oracle)
{
    std::FILE* log = cr_context.out();
    const auto srgb = unpackRgb(oracle::cmyk_srgb_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "CMYK float", srgb), oracle::cmyk_srgb_u8_to_cmyk_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "cmyk float", srgb), oracle::cmyk_srgb_u8_to_inverted_f32, log) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("R'G'B' u8", "CMYK u8", srgb), oracle::cmyk_srgb_u8_to_cmyk_u8, log) == 0);
    // ProPhoto: its linear RGB goes to scRGB first (S5).
    const auto pro = unpackRgb(oracle::cmyk_prophoto_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "CMYK float-ProPhoto", pro),
                           oracle::cmyk_prophoto_u8_to_cmyk_f32, log) == 0);
    const auto linear = floats(oracle::cmyk_linear_f32_inputs);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "CMYK float", linear), oracle::cmyk_linear_f32_to_cmyk_f32, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "cmyk float", linear), oracle::cmyk_linear_f32_to_inverted_f32, log) ==
             0);
}

CR_TEST(cmyk_to_rgb_matches_oracle)
{
    std::FILE* log = cr_context.out();
    const auto inks = floats(oracle::cmyk_f32_inputs);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("CMYK float", "R'G'B' u8", inks), oracle::cmyk_f32_to_srgb_u8, log) == 0);
    CR_CHECK(bitMismatches(convert<float>("CMYK float", "RGB float", inks), oracle::cmyk_f32_to_linear_f32, log) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("CMYK float", "R'G'B' u8-ProPhoto", inks),
                            oracle::cmyk_f32_to_prophoto_u8, log) == 0);
}

CR_TEST(cmyk_follows_the_reference_conventions)
{
    // Inks are 1 - R etc. with full black replacement; opaque black is the rich
    // black (1, 1, 1, 1), which the inverted models store as (0, 0, 0, 0).
    const std::vector<double> rgb{0, 0, 0, 1, 1, 1, 1, 0, 0, 0.25, 0.25, 0.25, 1.5, 0.75, 0};
    const std::vector<double> inks{1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0.75, 0, 0.5, 1, -0.5};
    const std::vector<double> inverted{0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0.25, 1, 0.5, 0, 1.5};
    CR_CHECK(sameBits(convert<double>("RGB double", "CMYK double", rgb), inks));
    CR_CHECK(sameBits(convert<double>("RGB double", "cmyk double", rgb), inverted));
    CR_CHECK(sameBits(convert<double>("cmyk double", "RGB double", inverted), rgb));

    // With K = 1 any inks are black, and a color whose channels are all <= 0
    // gets K = 1 (its inks stay 1 - R etc.) and comes back black.
    CR_CHECK(sameBits(convert<double>("CMYK double", "RGB double", std::vector<double>{0.3, 0.6, 0.9, 1.0}),
                      std::vector<double>{0, 0, 0}));
    const auto dark = convert<double>("RGB double", "cmyk double", std::vector<double>{-0.2, -0.1, 0.0});
    CR_CHECK(sameBits(dark, std::vector<double>{-0.2, -0.1, 0.0, 0.0}));
    CR_CHECK(sameBits(convert<double>("cmyk double", "RGB double", dark), std::vector<double>{-0.0, -0.0, 0.0}));

    // The associated models store their values times alpha: half-transparent
    // black is 0.5 per ink in CaMaYaKaA and 0 in camayakaA; white the reverse.
    const std::vector<double> rgba{0, 0, 0, 0.5, 1, 1, 1, 0.5};
    CR_CHECK(sameBits(convert<double>("RGBA double", "CaMaYaKaA double", rgba),
                      std::vector<double>{0.5, 0.5, 0.5, 0.5, 0.5, 0, 0, 0, 0, 0.5}));
    CR_CHECK(sameBits(convert<double>("RGBA double", "camayakaA double", rgba),
                      std::vector<double>{0, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}));
    // Near alpha 0 the alpha floor applies, so the inks survive alpha 0.
    const auto clear = convert<double>("CMYKA double", "CaMaYaKaA double", std::vector<double>{0.2, 0.4, 0.6, 0.8, 0.0});
    CR_CHECK(sameBits(clear, std::vector<double>{0.2 * 0x1p-16, 0.4 * 0x1p-16, 0.6 * 0x1p-16, 0.8 * 0x1p-16, 0.0}));
    CR_CHECK(sameBits(convert<double>("CaMaYaKaA double", "CMYKA double", clear),
                      std::vector<double>{0.2, 0.4, 0.6, 0.8, 0.0}));
    // Inks are not linear in RGB, so association is never carried across:
    // associated CMYK to associated RGB and back equal the routes through the
    // straight models.
    const std::vector<double> associated_inks{0.15, 0.3, 0.45, 0.25, 0.5};
    const auto direct = convert<double>("CaMaYaKaA double", "RaGaBaA double", associated_inks);
    const auto staged = convert<double>(
        "RGBA double", "RaGaBaA double",
        convert<double>("CMYKA double", "RGBA double", convert<double>("CaMaYaKaA double", "CMYKA double", associated_inks)));
    CR_CHECK(sameBits(direct, staged) && direct.size() == 4 && direct[3] == 0.5);
    const auto back = convert<double>("RaGaBaA double", "CaMaYaKaA double", direct);
    const auto back_staged = convert<double>(
        "CMYKA double", "CaMaYaKaA double",
        convert<double>("RGBA double", "CMYKA double", convert<double>("RaGaBaA double", "RGBA double", direct)));
    CR_CHECK(sameBits(back, back_staged));
}

CR_TEST(cmyk_ignores_the_format_space)
{
    // Its RGB side is linear scRGB in every space (S5): the bytes do not depend
    // on the CMYK format's space, and converting into a space goes through scRGB.
    const std::vector<std::uint8_t> rgb{200, 40, 90, 10, 250, 128, 0, 0, 0, 255, 255, 255};
    const auto in_srgb = convert<std::uint8_t>("R'G'B' u8", "CMYK u8", rgb);
    CR_CHECK(in_srgb == convert<std::uint8_t>("R'G'B' u8", "CMYK u8-Rec2020", rgb));
    CR_CHECK(in_srgb == convert<std::uint8_t>("CMYK u8-ProPhoto", "CMYK u8-Apple", in_srgb));
    const std::vector<double> inks{0.1, 0.5, 0.9, 0.2, 0.7, 0.0, 0.3, 0.6};
    const auto direct = convert<double>("CMYK double-Adobish", "RGB double-Rec2020", inks);
    const auto staged =
        convert<double>("RGB double-scRGB", "RGB double-Rec2020", convert<double>("CMYK double", "RGB double-scRGB", inks));
    CR_CHECK(sameBits(direct, staged));
    // scRGB has sRGB's primaries, so linear sRGB needs no matrix.
    CR_CHECK(sameBits(convert<double>("CMYK double", "RGB double", inks),
                      convert<double>("CMYK double", "RGB double-scRGB", inks)));
}

CR_TEST(rgb_round_trips_through_cmyk)
{
    // Every color with max(R, G, B) > 0 round-trips. The inverted form keeps
    // w = max(R, G, B) itself; the ink form stores 1 - w, whose rounding (up to
    // 2^-53) comes back multiplied by R / w, and association adds a division
    // and a multiplication. Relative to the largest channel the error is
    // bounded by 8 x 2^-53 (1 + max |R / w|); measured worst 4.26 of that
    // (CaMaYaKaA), 0.68 for cmykA.
    std::vector<double> colors;
    std::uint32_t state = 0x510E527Fu;
    for (int i = 0; i < 4 * 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        colors.push_back(i % 4 == 3 ? 0.1 + 0.9 * (state >> 8) / 16777216.0 : -0.5 + 2.0 * (state >> 8) / 16777216.0);
    }
    // Alpha comes back unchanged, and a color with max(R, G, B) <= 0 (K = 1)
    // comes back black, exactly; each is checked on its own.
    const std::size_t pixels = colors.size() / 4;
    for (const char* model : {"cmykA", "camayakaA", "CMYKA", "CaMaYaKaA"}) {
        const std::string name = std::string(model) + " double";
        const auto values = convert<double>("RGBA double", name.c_str(), colors);
        const auto back = convert<double>(name.c_str(), "RGBA double", values);
        CR_CHECK(values.size() == pixels * 5 && back.size() == colors.size());
        if (back.size() != colors.size()) {
            continue;
        }
        std::size_t positive = 0, nonpositive = 0;
        int alpha_changed = 0, not_black = 0;
        double worst_bound_ratio = 0.0;
        for (std::size_t p = 0; p < pixels; ++p) {
            const double* c = &colors[p * 4];
            const double* b = &back[p * 4];
            const double w = std::fmax(c[0], std::fmax(c[1], c[2]));
            const double size = std::fmax(std::fabs(c[0]), std::fmax(std::fabs(c[1]), std::fabs(c[2])));
            alpha_changed += b[3] != c[3];
            if (w > 0.0) {
                ++positive;
                for (std::size_t k = 0; k < 3; ++k) {
                    const double error = std::fabs(b[k] - c[k]) / size;
                    worst_bound_ratio = worstOf(worst_bound_ratio, error / (0x1p-53 * (1.0 + size / w)));
                }
            } else {
                ++nonpositive;
                not_black += b[0] != 0.0 || b[1] != 0.0 || b[2] != 0.0;
            }
        }
        std::fprintf(cr_context.out(), "  %s round trip: %zu colors, worst %.3g of 2^-53 (1 + max |R / w|)\n", model,
                     positive, worst_bound_ratio);
        CR_CHECK(alpha_changed == 0);
        CR_CHECK(nonpositive > 0 && not_black == 0);
        CR_CHECK(positive > 0 && worst_bound_ratio < 8.0);
    }
}

CR_TEST(cmyk_models_convert_among_themselves_without_rgb)
{
    // Inks change form and association only; with K = 1, where RGB would lose
    // them, they are kept.
    const std::vector<double> inks{0.3, 0.6, 0.9, 1.0, 0.25, 0.0, 1.0, 0.5};
    CR_CHECK(sameBits(convert<double>("CMYK double", "cmyk double", inks),
                      std::vector<double>{1 - 0.3, 1 - 0.6, 1 - 0.9, 0.0, 0.75, 1.0, 0.0, 0.5}));
    CR_CHECK(sameBits(convert<double>("cmyk double", "CMYK double", convert<double>("CMYK double", "cmyk double", inks)),
                      std::vector<double>{1 - (1 - 0.3), 1 - (1 - 0.6), 1 - (1 - 0.9), 1.0, 0.25, 0.0, 1.0, 0.5}));
    // Every u8 code c in one form is 255 - c in the other.
    std::vector<std::uint8_t> codes;
    for (int c = 0; c < 256; ++c) {
        codes.insert(codes.end(), 4, static_cast<std::uint8_t>(c));
    }
    const auto flipped = convert<std::uint8_t>("CMYK u8", "cmyk u8", codes);
    bool exact = flipped.size() == codes.size();
    for (std::size_t i = 0; exact && i < codes.size(); ++i) {
        exact = flipped[i] == 255 - codes[i];
    }
    CR_CHECK(exact);
    // Association and alpha: missing alpha reads as 1; associated forms
    // convert into each other through the straight values.
    CR_CHECK(sameBits(convert<double>("CMYK double", "CaMaYaKaA double", std::vector<double>{0.3, 0.6, 0.9, 1.0}),
                      std::vector<double>{0.3, 0.6, 0.9, 1.0, 1.0}));
    CR_CHECK(sameBits(convert<double>("CaMaYaKaA double", "camayakaA double", std::vector<double>{0.15, 0.3, 0.45, 0.5, 0.5}),
                      std::vector<double>{(1 - 0.3) * 0.5, (1 - 0.6) * 0.5, (1 - 0.9) * 0.5, 0.0, 0.5}));
    // Within one model only the type changes.
    CR_CHECK(convert<std::uint8_t>("CaMaYaKaA float", "CaMaYaKaA u8", std::vector<float>{0.2f, 0.4f, 0.6f, 0.8f, 0.4f}) ==
             (std::vector<std::uint8_t>{51, 102, 153, 204, 102}));
}

CR_TEST(cmyk_neutrals_are_exact)
{
    // A gray is the inverted (1, 1, 1, v), inks (0, 0, 0, 1 - v); back, and to
    // CIE models, it is exactly neutral.
    const std::vector<double> gray{0.0, 0.2, 0.5, 1.0};
    CR_CHECK(sameBits(convert<double>("Y double", "cmyk double", gray),
                      std::vector<double>{0, 0, 0, 0, 1, 1, 1, 0.2, 1, 1, 1, 0.5, 1, 1, 1, 1}));
    CR_CHECK(sameBits(convert<double>("Y double", "CMYK double", gray),
                      std::vector<double>{1, 1, 1, 1, 0, 0, 0, 1 - 0.2, 0, 0, 0, 0.5, 0, 0, 0, 0}));
    const std::vector<double> neutral_inks{0, 0, 0, 0.3, 0, 0, 0, 0.9};
    const auto lab = convert<double>("CMYK double", "CIE Lab double", neutral_inks);
    CR_CHECK(lab.size() == 6 && lab[1] == 0.0 && lab[2] == 0.0 && lab[4] == 0.0 && lab[5] == 0.0);
    // To gray it is R = G = B = 0.4, so its luminance with scRGB's weights.
    const auto y = convert<double>("cmyk double", "Y double", std::vector<double>{1, 1, 1, 0.4});
    CR_CHECK(y.size() == 1 && std::fabs(y[0] - 0.4) < 1e-16);
}

CR_TEST(cmyk_nan_and_infinite_components)
{
    for (const std::vector<double>& bad : {std::vector<double>{nan_value, 0.2, 0.3}, std::vector<double>{inf, 0.0, 0.0},
                                           std::vector<double>{0.2, -inf, 0.3}}) {
        CR_CHECK(allNan(convert<double>("RGB double", "CMYK double", bad), 4));
        CR_CHECK(allNan(convert<double>("RGB double", "cmyk double", bad), 4));
    }
    for (const std::vector<double>& bad : {std::vector<double>{nan_value, 0.2, 0.3, 0.4}, std::vector<double>{0.1, 0.2, 0.3, inf}}) {
        CR_CHECK(allNan(convert<double>("CMYK double", "RGB double", bad), 3));
    }
}

CR_TEST(cmyk_formats_have_their_catalogue_layouts)
{
    struct Expected {
        const char* model;
        ColorFamily family;
        std::size_t components;
        Alpha alpha;
        const char* third; // the yellow component's name
    };
    const Expected models[] = {
        {"CMYK", ColorFamily::cmyk, 4, Alpha::none, "Yellow"},
        {"CMYKA", ColorFamily::cmyk, 5, Alpha::straight, "Yellow"},
        {"CaMaYaKaA", ColorFamily::cmyk, 5, Alpha::associated, "Yea"},
        {"cmyk", ColorFamily::cmyk_inverted, 4, Alpha::none, "yellow"},
        {"cmykA", ColorFamily::cmyk_inverted, 5, Alpha::straight, "yellow"},
        {"camayakaA", ColorFamily::cmyk_inverted, 5, Alpha::associated, "ya"},
    };
    int formats = 0;
    for (const Expected& e : models) {
        for (const char* type : {"u8", "u16", "u32", "half", "float", "double"}) {
            const std::string name = std::string(e.model) + " " + type;
            const auto f = PixelFormat::fromName(name);
            const bool expected = !(e.alpha == Alpha::associated && std::string(type) == "u32");
            CR_CHECK(f.has_value() == expected);
            if (!f) {
                continue;
            }
            ++formats;
            CR_CHECK(f->colorFamily() == e.family && f->encoding() == Encoding::none && f->alpha() == e.alpha);
            CR_CHECK(f->componentCount() == e.components);
            CR_CHECK(f->descriptor().model->components[2]->name == e.third);
            CR_CHECK(PixelFormat::fromName(name + "-ProPhoto").has_value());
        }
    }
    CR_CHECK(formats == 34);
    CR_CHECK(PixelFormat::fromName("CMYKA u16")->bytesPerPixel() == 10);
}
