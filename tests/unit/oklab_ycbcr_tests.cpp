// Oklab and Y'CbCr (Milestone 5): conversions against the oracle
// (tests/data/oklab_ycbcr_oracle_data.hpp), the limited-range Y'CbCr codes,
// exact neutrals, same-model copies and the catalogue layouts.

#include "support/conversion_checks.hpp"
#include "support/test_harness.hpp"

#include "core/descriptors.hpp"
#include "data/oklab_ycbcr_oracle_data.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using chromaroute::ColorFamily;
using chromaroute::Encoding;
using chromaroute::PixelFormat;

namespace oracle = chromaroute::test::oracle;
using namespace chromaroute::test;

namespace {

bool sameBits(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

// Values on both sides of every code threshold of one channel, through
// "Y'CbCr float" (binary32: the exact rounding) and "Y'CbCr double" (the
// defined rounding) to "Y'CbCr u8"; the other channels hold mid values.
int thresholdMismatches(std::size_t channel, std::FILE* log)
{
    const double lo = channel == 0 ? 0.0 : -0.5;
    const double hi = channel == 0 ? 1.0 : 0.5;
    const long c_lo = 16;
    const long c_hi = channel == 0 ? 235 : 240;
    const double span = static_cast<double>(c_hi - c_lo);
    std::vector<float> floats32, pixels32;
    std::vector<double> doubles, pixels64;
    for (long k = 0; k < c_hi - c_lo; ++k) {
        const double at = lo + (2.0 * k + 1.0) * (hi - lo) / (2.0 * span);
        const float at32 = static_cast<float>(at);
        for (float x : {std::nextafter(at32, -1e9f), at32, std::nextafter(at32, 1e9f)}) {
            floats32.push_back(x);
        }
        double v = at;
        for (int step = 0; step < 3; ++step) {
            v = std::nextafter(v, -1e9);
        }
        for (int step = 0; step < 7; ++step, v = std::nextafter(v, 1e9)) {
            doubles.push_back(v);
        }
    }
    for (float x : floats32) {
        pixels32.insert(pixels32.end(), {channel == 0 ? x : 0.5f, channel == 1 ? x : 0.0f, channel == 2 ? x : 0.0f});
    }
    for (double x : doubles) {
        pixels64.insert(pixels64.end(), {channel == 0 ? x : 0.5, channel == 1 ? x : 0.0, channel == 2 ? x : 0.0});
    }
    const auto codes32 = convert<std::uint8_t>("Y'CbCr float", "Y'CbCr u8", pixels32);
    const auto codes64 = convert<std::uint8_t>("Y'CbCr double", "Y'CbCr u8", pixels64);
    int mismatches = codes32.size() != pixels32.size() || codes64.size() != pixels64.size();
    for (std::size_t i = 0; mismatches == 0 && i < floats32.size(); ++i) {
        const long expected = exactRangeCode(floats32[i], lo, hi, c_lo, c_hi);
        if (codes32[i * 3 + channel] != expected && ++mismatches <= 5) {
            std::fprintf(log, "  channel %zu float %a -> %d, expected %ld\n", channel, static_cast<double>(floats32[i]),
                         codes32[i * 3 + channel], expected);
        }
    }
    for (std::size_t i = 0; i < doubles.size() && codes64.size() == pixels64.size(); ++i) {
        const long expected = definedRangeCode(doubles[i], lo, hi, c_lo, c_hi);
        if (codes64[i * 3 + channel] != expected && ++mismatches <= 5) {
            std::fprintf(log, "  channel %zu double %a -> %d, expected %ld\n", channel, doubles[i], codes64[i * 3 + channel],
                         expected);
        }
    }
    return mismatches;
}

} // namespace

CR_TEST(rgb_to_oklab_matches_oracle)
{
    std::FILE* out = cr_context.out();
    const auto srgb = unpackRgb(oracle::oklab_srgb_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "Oklab float", srgb), oracle::srgb_u8_to_oklab_f32, out) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "Oklab float", unpackRgb(oracle::oklab_prophoto_u8_inputs)),
                           oracle::prophoto_u8_to_oklab_f32, out) == 0);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "Oklab float", floats(oracle::oklab_linear_f32_inputs)),
                           oracle::linear_f32_to_oklab_f32, out) == 0);
    // Oklab values do not depend on the format's space.
    CR_CHECK(convert<float>("R'G'B' u8", "Oklab float-Rec2020", srgb) == convert<float>("R'G'B' u8", "Oklab float", srgb));
}

CR_TEST(oklab_to_xyz_and_rgb_matches_oracle)
{
    std::FILE* out = cr_context.out();
    const auto lab = floats(oracle::oklab_f32_inputs);
    CR_CHECK(bitMismatches(convert<float>("Oklab float", "CIE XYZ float", lab), oracle::oklab_f32_to_xyz_f32, out) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("Oklab float", "R'G'B' u8", lab), oracle::oklab_f32_to_srgb_u8, out) == 0);
    CR_CHECK(convert<std::uint8_t>("Oklab float-ProPhoto", "R'G'B' u8", lab) ==
             convert<std::uint8_t>("Oklab float", "R'G'B' u8", lab));
}

CR_TEST(oklab_white_neutrals_and_round_trips)
{
    // With the CSS Color 4 matrices sRGB white is (1, 0, 0) to double rounding,
    // and every gray has a = b = 0 to that precision. a and b are sums of three
    // terms of up to 2.43 L (M2's entries) that cancel for a gray, so rounding
    // leaves a residue of order 1e-15 L; the bound is 4e-15 L, measured worst
    // 1.36e-15 L (Y = 0.73).
    const auto white = convert<double>("R'G'B' u8", "Oklab double", std::vector<std::uint8_t>{255, 255, 255});
    CR_CHECK(white.size() == 3 && std::fabs(white[0] - 1.0) < 1e-15 && std::fabs(white[1]) < 1e-15 &&
             std::fabs(white[2]) < 1e-15);
    std::vector<double> grays;
    for (int i = 1; i <= 100; ++i) {
        grays.push_back(i / 100.0);
    }
    const auto gray_lab = convert<double>("Y double", "Oklab double", grays);
    double worst = 0;
    for (std::size_t p = 0; p < grays.size(); ++p) {
        worst = worstOf(worstOf(worst, std::fabs(gray_lab[p * 3 + 1]) / gray_lab[p * 3]),
                        std::fabs(gray_lab[p * 3 + 2]) / gray_lab[p * 3]);
    }
    CR_CHECK(worst < 4e-15);

    // Round trips through RGB and through CIE Lab, in double.
    std::vector<double> rgb;
    for (int i = 0; i < 300; ++i) {
        rgb.insert(rgb.end(), {(i % 10) / 9.0, ((i / 10) % 10) / 9.0, (i % 7) / 6.0});
    }
    worst = 0;
    for (const char* via : {"Oklab double", "OklabA double"}) {
        const auto back = convert<double>(via, "RGB double", convert<double>("RGB double", via, rgb));
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            worst = worstOf(worst, back.size() == rgb.size() ? std::fabs(back[i] - rgb[i]) : 1.0);
        }
    }
    const auto lab = convert<double>("RGB double", "CIE Lab double", rgb);
    const auto lab_back = convert<double>("Oklab double", "CIE Lab double", convert<double>("CIE Lab double", "Oklab double", lab));
    for (std::size_t i = 0; i < lab.size(); ++i) {
        worst = worstOf(worst, lab_back.size() == lab.size() ? std::fabs(lab_back[i] - lab[i]) / 100.0 : 1.0);
    }
    CR_CHECK(worst < 1e-13);

    // Alpha is straight and copied; "Oklab alpha float" is OklabA in float.
    const auto with_alpha = convert<float>("RGBA float", "Oklab alpha float", std::vector<float>{0.2f, 0.4f, 0.6f, 0.25f});
    CR_CHECK(with_alpha.size() == 4 && with_alpha[3] == 0.25f);
    const std::vector<double> values{0.5, 0.1, -0.2, 0.75};
    const auto narrowed = convert<float>("OklabA double", "Oklab alpha float", values);
    CR_CHECK((narrowed == std::vector<float>{0.5f, 0.1f, -0.2f, 0.75f}));
}

CR_TEST(rgb_to_ycbcr_matches_oracle)
{
    std::FILE* out = cr_context.out();
    const auto rgb = unpackRgb(oracle::ycbcr_srgb_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "Y'CbCr float", rgb), oracle::srgb_u8_to_ycbcr_f32, out) == 0);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "Y'CbCr709 float", rgb), oracle::srgb_u8_to_ycbcr709_f32, out) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("R'G'B' u8", "Y'CbCr u8", rgb), oracle::srgb_u8_to_ycbcr_u8, out) == 0);
    // Known BT.601 codes: white, black, red, blue.
    const auto codes = convert<std::uint8_t>("R'G'B' u8", "Y'CbCr u8",
                                             std::vector<std::uint8_t>{255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 0, 255});
    CR_CHECK((codes == std::vector<std::uint8_t>{235, 128, 128, 16, 128, 128, 81, 90, 240, 41, 240, 110}));
}

CR_TEST(ycbcr_uses_the_perceptual_encoding_of_its_space)
{
    // In ProPhoto, Y'CbCr comes from ProPhoto's linear RGB with the sRGB curve
    // (S4), not from its own 1.8 TRC; it equals the route through R~G~B~.
    const auto rgb = unpackRgb(oracle::ycbcr_prophoto_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "Y'CbCr float-ProPhoto", rgb),
                           oracle::prophoto_u8_to_ycbcr_f32, cr_context.out()) == 0);
    const auto direct = convert<double>("R'G'B' u8-ProPhoto", "Y'CbCr double-ProPhoto", rgb);
    const auto staged = convert<double>("R~G~B~ double-ProPhoto", "Y'CbCr double-ProPhoto",
                                        convert<double>("R'G'B' u8-ProPhoto", "R~G~B~ double-ProPhoto", rgb));
    CR_CHECK(sameBits(direct, staged));
}

CR_TEST(ycbcr_to_rgb_matches_oracle)
{
    std::FILE* out = cr_context.out();
    CR_CHECK(codeMismatches(convert<std::uint8_t>("Y'CbCr u8", "R'G'B' u8", unpackRgb(oracle::ycbcr_u8_inputs)),
                            oracle::ycbcr_u8_to_srgb_u8, out) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("Y'CbCr709 float", "R'G'B' u8", floats(oracle::ycbcr709_f32_inputs)),
                            oracle::ycbcr709_f32_to_srgb_u8, out) == 0);
}

CR_TEST(ycbcr_neutrals_are_exact)
{
    // R' = G' = B' gives Y' = R~ and Cb = Cr = 0 exactly, for both weights, in
    // sRGB and in ProPhoto; Cb = Cr = 0 gives R~ = G~ = B~ = Y' exactly.
    std::vector<std::uint8_t> grays, rgb_grays;
    for (int v = 0; v < 256; ++v) {
        grays.push_back(static_cast<std::uint8_t>(v));
        rgb_grays.insert(rgb_grays.end(), 3, static_cast<std::uint8_t>(v));
    }
    int wrong = 0;
    for (const std::string space : {"", "-ProPhoto"}) {
        const auto tilde = convert<double>(("R'G'B' u8" + space).c_str(), ("R~G~B~ double" + space).c_str(), rgb_grays);
        for (const char* model : {"Y'CbCr double", "Y'CbCr709 double"}) {
            const std::string target = model + space;
            const auto from_rgb = convert<double>(("R'G'B' u8" + space).c_str(), target.c_str(), rgb_grays);
            const auto from_gray = convert<double>(("Y' u8" + space).c_str(), target.c_str(), grays);
            const auto back = convert<double>(target.c_str(), ("R~G~B~ double" + space).c_str(), from_rgb);
            wrong += from_rgb.size() != 768 || from_gray.size() != 768 || back.size() != 768;
            for (std::size_t p = 0; wrong == 0 && p < 256; ++p) {
                wrong += bitsDiffer(from_rgb[p * 3], tilde[p * 3]) + bitsDiffer(from_rgb[p * 3 + 1], 0.0) +
                         bitsDiffer(from_rgb[p * 3 + 2], 0.0);
                wrong += bitsDiffer(from_gray[p * 3], from_rgb[p * 3]) + (from_gray[p * 3 + 1] != 0.0) +
                         (from_gray[p * 3 + 2] != 0.0);
                for (std::size_t i = 0; i < 3; ++i) {
                    wrong += bitsDiffer(back[p * 3 + i], tilde[p * 3]);
                }
            }
        }
    }
    CR_CHECK(wrong == 0);
}

CR_TEST(ycbcr_u8_uses_limited_range_codes)
{
    // Decoding: luma codes 16..235 cover 0..1 and chroma codes 16..240 cover
    // -0.5..0.5, each correctly rounded; codes outside clamp to the ends.
    std::vector<std::uint8_t> codes;
    for (int c = 0; c < 256; ++c) {
        codes.insert(codes.end(), 3, static_cast<std::uint8_t>(c));
    }
    const auto values = convert<double>("Y'CbCr u8", "Y'CbCr double", codes);
    CR_CHECK(values.size() == codes.size());
    int wrong = 0;
    for (std::size_t c = 0; c < values.size() / 3; ++c) {
        const double luma = values[c * 3];
        wrong += c < 16 ? luma != 0.0 : c > 235 ? luma != 1.0 : !nearestQuotient(luma, c - 16.0, 219.0);
        for (std::size_t i = 1; i < 3; ++i) {
            const double chroma = values[c * 3 + i];
            wrong += c < 16 ? chroma != -0.5 : c > 240 ? chroma != 0.5 : !nearestQuotient(chroma, c - 128.0, 224.0);
        }
    }
    CR_CHECK(wrong == 0);

    // Encoding at every threshold: exact for binary32, the defined rounding for double.
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CR_CHECK(thresholdMismatches(channel, cr_context.out()) == 0);
    }
    // NaN gives the lowest code of the range; out-of-range values clamp.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto clamped = convert<std::uint8_t>("Y'CbCr double", "Y'CbCr u8",
                                               std::vector<double>{nan, nan, nan, -1.0, -1.0, -1.0, 2.0, 2.0, 2.0});
    CR_CHECK((clamped == std::vector<std::uint8_t>{16, 16, 16, 16, 16, 16, 235, 240, 240}));
}

CR_TEST(ycbcr_copies_within_a_model_and_follows_its_space)
{
    // Formats of one Y'CbCr model copy values: double to float is plain narrowing.
    std::vector<double> ycc;
    for (int i = 0; i < 200; ++i) {
        ycc.insert(ycc.end(), {i / 199.0, std::sin(i * 0.37) * 0.5, std::cos(i * 0.53) * 0.5, i / 400.0});
    }
    std::vector<float> narrowed;
    for (double v : ycc) {
        narrowed.push_back(static_cast<float>(v));
    }
    CR_CHECK(convert<float>("Y'CbCrA double", "Y'CbCrA float", ycc) == narrowed);
    CR_CHECK(convert<float>("Y'CbCrA709 double", "Y'CbCrA709 float", ycc) == narrowed);

    // Between BT.601 and BT.709, or into another space, the values go through
    // R~G~B~: the direct conversion equals the staged one bit for bit.
    std::vector<double> three;
    for (std::size_t p = 0; p < ycc.size() / 4; ++p) {
        three.insert(three.end(), {ycc[p * 4], ycc[p * 4 + 1], ycc[p * 4 + 2]});
    }
    for (const char* target : {"Y'CbCr709 double", "Y'CbCr double-Rec2020", "Y'CbCr709 double-ProPhoto"}) {
        const std::string staging = std::string("R~G~B~ double") + (std::strchr(target, '-') ? std::strchr(target, '-') : "");
        const auto direct = convert<double>("Y'CbCr double", target, three);
        const auto staged = convert<double>(staging.c_str(), target,
                                            convert<double>("R~G~B~ double", staging.c_str(),
                                                            convert<double>("Y'CbCr double", "R~G~B~ double", three)));
        CR_CHECK(sameBits(direct, staged));
    }
    CR_CHECK(convert<double>("Y'CbCr double", "Y'CbCr709 double", three) != three);
}

CR_TEST(oklab_and_ycbcr_formats_have_their_catalogue_layouts)
{
    struct Expected {
        const char* name;
        ColorFamily family;
        Encoding encoding;
        const char* model;
        std::size_t components, bytes;
    };
    const Expected cases[] = {
        {"Oklab float", ColorFamily::oklab, Encoding::none, "Oklab", 3, 12},
        {"Oklab double", ColorFamily::oklab, Encoding::none, "Oklab", 3, 24},
        {"OklabA double", ColorFamily::oklab, Encoding::none, "OklabA", 4, 32},
        {"Oklab alpha float", ColorFamily::oklab, Encoding::none, "OklabA", 4, 16},
        {"Y'CbCr float", ColorFamily::ycbcr, Encoding::perceptual, "Y'CbCr", 3, 12},
        {"Y'CbCrA double", ColorFamily::ycbcr, Encoding::perceptual, "Y'CbCrA", 4, 32},
        {"Y'CbCr709 double", ColorFamily::ycbcr709, Encoding::perceptual, "Y'CbCr709", 3, 24},
        {"Y'CbCrA709 float", ColorFamily::ycbcr709, Encoding::perceptual, "Y'CbCrA709", 4, 16},
        {"Y'CbCr u8", ColorFamily::ycbcr, Encoding::perceptual, "Y'CbCr", 3, 3},
        {"Y'CbCr u8-Rec2020", ColorFamily::ycbcr, Encoding::perceptual, "Y'CbCr", 3, 3},
    };
    int problems = 0;
    for (const Expected& e : cases) {
        const auto f = PixelFormat::fromName(e.name);
        if (!f) {
            ++problems;
            std::fprintf(cr_context.out(), "  missing %s\n", e.name);
            continue;
        }
        problems += f->colorFamily() != e.family || f->encoding() != e.encoding;
        problems += f->descriptor().model->name != e.model;
        problems += f->componentCount() != e.components || f->bytesPerPixel() != e.bytes;
        problems += f->hasAlpha() != (e.components == 4) || f->name() != e.name;
    }
    CR_CHECK(problems == 0);
    // "Y'CbCr u8" interleaves Y', Cb, Cr at full resolution, each with its own type.
    const auto& u8 = PixelFormat::fromName("Y'CbCr u8")->descriptor();
    CR_CHECK(u8.components[0].type->name == "u8-luma" && u8.components[1].type->name == "u8-chroma" &&
             u8.components[2].type->name == "u8-chroma");
    CR_CHECK(u8.components[0].model_index == 0 && u8.components[1].model_index == 1 && u8.components[2].model_index == 2);
}

CR_TEST(oklab_and_ycbcr_keep_finite_results_at_every_exponent)
{
    // Oklab is homogeneous of degree 1/3: XYZ scaled by 2^(3j) gives Oklab scaled
    // by 2^j, bit for bit, also where M1 XYZ would overflow or the cube roots
    // would see subnormals; the inverse likewise.
    std::vector<double> xyz;
    for (int i = 0; i < 64; ++i) {
        xyz.insert(xyz.end(), {0.5 + 0.45 * std::sin(i * 0.9), 0.5 + 0.45 * std::cos(i * 1.7), 0.5 + 0.4 * std::sin(i * 2.3)});
    }
    const auto lab = convert<double>("CIE XYZ double", "Oklab double", xyz);
    int differ = 0;
    for (int j : {-340, -200, -100, 100, 200, 340}) {
        std::vector<double> scaled_xyz, scaled_lab;
        for (double v : xyz) {
            scaled_xyz.push_back(std::ldexp(v, 3 * j));
        }
        for (double v : lab) {
            scaled_lab.push_back(std::ldexp(v, j));
        }
        const auto forward = convert<double>("CIE XYZ double", "Oklab double", scaled_xyz);
        const auto inverse = convert<double>("Oklab double", "CIE XYZ double", scaled_lab);
        const auto plain_inverse = convert<double>("Oklab double", "CIE XYZ double", lab);
        for (std::size_t i = 0; i < lab.size(); ++i) {
            differ += bitsDiffer(forward[i], scaled_lab[i]) + bitsDiffer(inverse[i], std::ldexp(plain_inverse[i], 3 * j));
        }
    }
    CR_CHECK(differ == 0);
    const auto huge = convert<double>("CIE XYZ double", "Oklab double", std::vector<double>{1.7e308, 1.7e308, 1.7e308});
    CR_CHECK(std::isfinite(huge[0]) && std::isfinite(huge[1]) && std::isfinite(huge[2]) &&
             std::fabs(huge[0] / 5.54e102 - 1.0) < 0.01);

    // Y'CbCr is linear: R~G~B~ scaled by 2^k gives Y'CbCr scaled by 2^k, bit for
    // bit, also where differences such as R' - G' would overflow.
    std::vector<double> rgb;
    for (int i = 0; i < 64; ++i) {
        rgb.insert(rgb.end(), {0.5 + 0.7 * std::sin(i * 0.9), 0.5 + 0.7 * std::cos(i * 1.7), 0.5 + 0.7 * std::sin(i * 2.3)});
    }
    const auto ycc = convert<double>("R~G~B~ double", "Y'CbCr double", rgb);
    const auto back = convert<double>("Y'CbCr double", "R~G~B~ double", ycc);
    differ = 0;
    for (int k : {-900, -300, 300, 900, 1023}) {
        std::vector<double> scaled_rgb, scaled_ycc;
        for (double v : rgb) {
            scaled_rgb.push_back(std::ldexp(v, k));
        }
        for (double v : ycc) {
            scaled_ycc.push_back(std::ldexp(v, k));
        }
        const auto forward = convert<double>("R~G~B~ double", "Y'CbCr double", scaled_rgb);
        const auto inverse = convert<double>("Y'CbCr double", "R~G~B~ double", scaled_ycc);
        for (std::size_t i = 0; i < ycc.size(); ++i) {
            differ += bitsDiffer(forward[i], scaled_ycc[i]) + bitsDiffer(inverse[i], std::ldexp(back[i], k));
        }
    }
    CR_CHECK(differ == 0);
    // Opposite-sign extremes: R' - G' overflows on its own, every result is finite.
    const auto opposite = convert<double>("R~G~B~ double", "Y'CbCr double", std::vector<double>{1e308, -1e308, 1e308});
    CR_CHECK(std::isfinite(opposite[0]) && std::isfinite(opposite[1]) && std::isfinite(opposite[2]) &&
             std::fabs(opposite[0] / -1.74e307 - 1.0) < 1e-12 && std::fabs(opposite[1] / 6.625e307 - 1.0) < 1e-3);
    // 1.402 Cr overflows on its own; R' = Y' + 1.402 Cr does not.
    const auto large_cr = convert<double>("Y'CbCr double", "R~G~B~ double", std::vector<double>{-0.5e308, -0.5e308, 1.3e308});
    CR_CHECK(std::isfinite(large_cr[0]) && std::isfinite(large_cr[1]) && std::isfinite(large_cr[2]) &&
             std::fabs(large_cr[0] / 1.3226e308 - 1.0) < 1e-12);
}
