// CIE models (Milestone 4): conversions against the oracle
// (tests/data/cie_oracle_data.hpp), the Lab integer encodings at every
// threshold, and the edge-case rules of docs/model_coverage.md §3.

#include "support/test_harness.hpp"

#include "data/cie_oracle_data.hpp"
#include "engine/cie.hpp"
#include "engine/transfer.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    std::vector<std::uint8_t> rgb;
    for (std::uint32_t p : packed) {
        rgb.insert(rgb.end(), {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8),
                               static_cast<std::uint8_t>(p >> 16)});
    }
    return rgb;
}

std::vector<float> floats(std::span<const std::uint32_t> bits)
{
    std::vector<float> out;
    for (std::uint32_t b : bits) {
        out.push_back(std::bit_cast<float>(b));
    }
    return out;
}

int bitMismatches(const std::vector<float>& got, std::span<const std::uint32_t> expected, std::FILE* log)
{
    if (got.size() != expected.size()) {
        return 1 << 20;
    }
    int mismatches = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::bit_cast<std::uint32_t>(got[i]) != expected[i] && ++mismatches <= 5) {
            std::fprintf(log, "  [%zu] got %a expected %a\n", i, static_cast<double>(got[i]),
                         static_cast<double>(std::bit_cast<float>(expected[i])));
        }
    }
    return mismatches;
}

template <typename T>
int codeMismatches(const std::vector<T>& got, std::span<const std::uint32_t> expected, std::FILE* log)
{
    if (got.size() != expected.size()) {
        return 1 << 20;
    }
    int mismatches = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i] != expected[i] && ++mismatches <= 5) {
            std::fprintf(log, "  [%zu] got %u expected %u\n", i, static_cast<unsigned>(got[i]),
                         static_cast<unsigned>(expected[i]));
        }
    }
    return mismatches;
}

// The exact code a binary32 value gets in an integer range encoding: round half
// up of (x - lo) * max / (hi - lo). It is decided in 64-bit integers on x's
// mantissa and exponent, not with the library's double arithmetic. Every value
// this test uses has |x| >= 2^-11, so x * 2^35 is an integer, and the products
// stay below 2^61.
long exactCode(float x, double lo, double hi, double max)
{
    if (!(x > lo)) {
        return 0;
    }
    if (x >= hi) {
        return static_cast<long>(max);
    }
    int exponent = 0;
    const double fraction = std::frexp(static_cast<double>(x), &exponent); // x = fraction * 2^exponent
    const int shift = exponent - 24 + 35;
    if (shift < 0) {
        return -1; // outside what this test uses: fails the comparison
    }
    const std::int64_t one = std::int64_t{1} << 35;
    const auto mantissa = static_cast<std::int64_t>(std::ldexp(fraction, 24));
    const auto range = static_cast<std::int64_t>(hi - lo);
    const std::int64_t twice_scaled = // 2 (x - lo) max, times 2^35
        2 * (mantissa * (std::int64_t{1} << shift) - static_cast<std::int64_t>(lo) * one) * static_cast<std::int64_t>(max);
    long code = 0;
    long high = static_cast<long>(max);
    while (code < high) { // largest k with 2 * scaled >= (2k - 1) * range, by bisection
        const long mid = (code + high + 1) / 2;
        if (twice_scaled >= (2 * std::int64_t{mid} - 1) * range * one) {
            code = mid;
        } else {
            high = mid - 1;
        }
    }
    return code;
}

// The defined code of a double value (docs/architecture.md, integer range
// types): s = (v - lo) * max evaluated in double, then round half up of
// s / (hi - lo), decided with exact comparisons (2 s and (2k - 1)(hi - lo) are
// exact in double).
long definedCode(double v, double lo, double hi, double max)
{
    if (!(v > lo)) {
        return 0;
    }
    if (v >= hi) {
        return static_cast<long>(max);
    }
    const double twice_scaled = 2.0 * ((v - lo) * max);
    long code = 0;
    long high = static_cast<long>(max);
    while (code < high) {
        const long mid = (code + high + 1) / 2;
        if (twice_scaled >= (2.0 * mid - 1.0) * (hi - lo)) {
            code = mid;
        } else {
            high = mid - 1;
        }
    }
    return code;
}

// Doubles within 3 ulps of every threshold of one Lab channel, "CIE Lab double"
// -> format, against definedCode.
template <typename Code>
int doubleEncodingMismatches(const char* format, double max, std::FILE* log)
{
    int mismatches = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double lo = channel == 0 ? 0.0 : -128.0;
        const double hi = channel == 0 ? 100.0 : 127.0;
        std::vector<double> values, lab;
        for (long k = 0; k < static_cast<long>(max); ++k) {
            double v = lo + (2.0 * k + 1.0) * (hi - lo) / (2.0 * max);
            for (int step = 0; step < 3; ++step) {
                v = std::nextafter(v, -1e9);
            }
            for (int step = 0; step < 7; ++step, v = std::nextafter(v, 1e9)) {
                values.push_back(v);
            }
        }
        for (double v : values) {
            lab.insert(lab.end(), {channel == 0 ? v : 50.0, channel == 1 ? v : 0.0, channel == 2 ? v : 0.0});
        }
        const auto encoded = convert<Code>("CIE Lab double", format, lab);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const long expected = definedCode(values[i], lo, hi, max);
            const long got = encoded.size() == lab.size() ? static_cast<long>(encoded[i * 3 + channel]) : -1L;
            if (got != expected && ++mismatches <= 5) {
                std::fprintf(log, "  %s channel %zu: %a -> %ld, expected %ld\n", format, channel, values[i], got,
                             expected);
            }
        }
    }
    return mismatches;
}

// True when d is a double nearest to n / max, for an integer n: the residuals
// d * max - n are exact under fma here, and neither neighbor of d has a smaller one.
bool nearestQuotient(double d, double n, double max)
{
    const auto residual = [&](double x) { return std::fabs(std::fma(x, max, -n)); };
    const double r = residual(d);
    return r <= residual(std::nextafter(d, -1e300)) && r <= residual(std::nextafter(d, 1e300));
}

int bitsDiffer(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) != std::bit_cast<std::uint64_t>(b);
}

// Floats on both sides of every threshold of one Lab channel, plus the expected codes.
void thresholdCases(double lo, double hi, double max, std::vector<float>& values, std::vector<long>& codes)
{
    for (long k = 0; k < static_cast<long>(max); ++k) {
        const float at = static_cast<float>(lo + (2.0 * k + 1.0) * (hi - lo) / (2.0 * max));
        for (float x : {std::nextafter(at, -1e9f), at, std::nextafter(at, 1e9f)}) {
            values.push_back(x);
            codes.push_back(exactCode(x, lo, hi, max));
        }
    }
}

template <typename Code>
int encodingMismatches(const char* format, double max, std::FILE* log)
{
    int mismatches = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double lo = channel == 0 ? 0.0 : -128.0;
        const double hi = channel == 0 ? 100.0 : 127.0;
        std::vector<float> values;
        std::vector<long> codes;
        thresholdCases(lo, hi, max, values, codes);
        std::vector<float> lab;
        for (float v : values) {
            lab.insert(lab.end(), {channel == 0 ? v : 50.0f, channel == 1 ? v : 0.0f, channel == 2 ? v : 0.0f});
        }
        const auto encoded = convert<Code>("CIE Lab float", format, lab);
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (encoded.size() != lab.size() || encoded[i * 3 + channel] != codes[i]) {
                if (++mismatches <= 5) {
                    std::fprintf(log, "  %s channel %zu: %a -> %ld, expected %ld\n", format, channel,
                                 static_cast<double>(values[i]),
                                 encoded.size() == lab.size() ? static_cast<long>(encoded[i * 3 + channel]) : -1L,
                                 codes[i]);
                }
            }
        }
    }
    return mismatches;
}

} // namespace

CR_TEST(srgb_u8_to_cie_lab_matches_oracle)
{
    const auto rgb = unpackRgb(oracle::srgb_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8", "CIE Lab float", rgb), oracle::srgb_u8_to_lab_f32,
                           cr_context.out()) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("R'G'B' u8", "CIE Lab u8", rgb), oracle::srgb_u8_to_lab_u8,
                            cr_context.out()) == 0);
    // CIE values do not depend on the destination format's space.
    CR_CHECK(convert<float>("R'G'B' u8", "CIE Lab float-ProPhoto", rgb) == convert<float>("R'G'B' u8", "CIE Lab float", rgb));
}

CR_TEST(prophoto_u8_to_cie_lab_matches_oracle)
{
    const auto rgb = unpackRgb(oracle::prophoto_u8_inputs);
    CR_CHECK(bitMismatches(convert<float>("R'G'B' u8-ProPhoto", "CIE Lab float", rgb), oracle::prophoto_u8_to_lab_f32,
                           cr_context.out()) == 0);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("R'G'B' u8-ProPhoto", "CIE Lab u8", rgb),
                            oracle::prophoto_u8_to_lab_u8, cr_context.out()) == 0);
}

CR_TEST(gray_is_achromatic_in_lab_and_lch)
{
    std::vector<std::uint8_t> gray(256), rgb;
    for (int v = 0; v < 256; ++v) {
        gray[v] = static_cast<std::uint8_t>(v);
        rgb.insert(rgb.end(), {gray[v], gray[v], gray[v]});
    }
    // A gray model source is neutral exactly: a = b = 0, chroma 0, hue 0.
    const auto from_gray = convert<float>("Y' u8", "CIE Lab float", gray);
    const auto lch = convert<float>("Y' u8", "CIE LCH(ab) float", gray);
    int mismatches = 0;
    for (int v = 0; v < 256; ++v) {
        mismatches += std::bit_cast<std::uint32_t>(from_gray[v * 3]) != oracle::srgb_gray_u8_to_l_f32[v];
        mismatches += from_gray[v * 3 + 1] != 0.0f || from_gray[v * 3 + 2] != 0.0f;
        mismatches += lch[v * 3 + 1] != 0.0f || lch[v * 3 + 2] != 0.0f || std::signbit(lch[v * 3 + 2]);
    }
    CR_CHECK(mismatches == 0);
    // An RGB gray takes the same neutral path (N8): the same L, and a = b = 0.
    const auto from_rgb = convert<float>("R'G'B' u8", "CIE Lab float", rgb);
    const auto rgb_lch = convert<float>("R'G'B' u8", "CIE LCH(ab) float", rgb);
    int wrong = 0;
    for (int v = 0; v < 256; ++v) {
        wrong += std::bit_cast<std::uint32_t>(from_rgb[v * 3]) != oracle::srgb_gray_u8_to_l_f32[v];
        wrong += from_rgb[v * 3 + 1] != 0.0f || from_rgb[v * 3 + 2] != 0.0f;
        wrong += rgb_lch[v * 3 + 1] != 0.0f || rgb_lch[v * 3 + 2] != 0.0f || std::signbit(rgb_lch[v * 3 + 2]);
    }
    CR_CHECK(wrong == 0);
    // In every built-in space and to every CIE model, an RGB gray converts
    // exactly as the gray-model gray does.
    int differ = 0;
    for (const std::string space : {"", "-scRGB", "-Rec2020", "-DisplayP3", "-Adobish", "-ProPhoto", "-Apple", "-ACEScg",
                                    "-ACES2065-1"}) {
        for (const char* model : {"CIE XYZ double", "CIE xyY double", "CIE Yuv double", "CIE Lab double", "CIE LCH(ab) double"}) {
            const auto as_rgb = convert<double>(("R'G'B' u8" + space).c_str(), model, rgb);
            const auto as_gray = convert<double>(("Y' u8" + space).c_str(), model, gray);
            differ += as_rgb.size() != as_gray.size() || std::memcmp(as_rgb.data(), as_gray.data(), as_rgb.size() * 8) != 0;
        }
    }
    CR_CHECK(differ == 0);
    // Straight color decides, also for an associated source; a pixel that is not
    // exactly neutral takes the matrix.
    const auto associated = convert<double>("RaGaBaA double", "CIE Lab alpha double",
                                            std::vector<double>{0.15, 0.15, 0.15, 0.5, 0.3, 0.3, 0.30000001, 1.0});
    CR_CHECK(associated.size() == 8 && associated[1] == 0.0 && associated[2] == 0.0 && associated[3] == 0.5);
    CR_CHECK(associated[5] != 0.0 || associated[6] != 0.0);

    // Lab and LCH values without chroma stay exactly neutral among Lab and LCH
    // formats (the guaranteed CIE paths; values through XYZ, xyY or Yuv are not).
    std::vector<double> lab_gray, lch_gray;
    for (int i = 0; i < 256; ++i) {
        lab_gray.insert(lab_gray.end(), {i * 100.0 / 255.0, 0.0, 0.0});
        lch_gray.insert(lch_gray.end(), {i * 100.0 / 255.0, 0.0, i * 1.40625});
    }
    const auto to_lch = convert<float>("CIE Lab double", "CIE LCH(ab) alpha float", lab_gray);
    const auto to_lab = convert<float>("CIE LCH(ab) double", "CIE Lab float", lch_gray);
    const auto to_u8 = convert<std::uint8_t>("CIE LCH(ab) double", "CIE Lab u8", lch_gray);
    const auto to_u16 = convert<std::uint16_t>("CIE LCH(ab) double", "CIE Lab u16", lch_gray);
    int not_neutral = to_lch.size() != 1024 || to_lab.size() != 768 || to_u8.size() != 768 || to_u16.size() != 768;
    for (std::size_t p = 0; not_neutral == 0 && p < 256; ++p) {
        not_neutral += to_lch[p * 4 + 1] != 0.0f || to_lch[p * 4 + 2] != 0.0f || std::signbit(to_lch[p * 4 + 2]);
        not_neutral += to_lab[p * 3 + 1] != 0.0f || to_lab[p * 3 + 2] != 0.0f;
        not_neutral += to_u8[p * 3 + 1] != 128 || to_u8[p * 3 + 2] != 128;
        not_neutral += to_u16[p * 3 + 1] != 32896 || to_u16[p * 3 + 2] != 32896;
    }
    CR_CHECK(not_neutral == 0);
}

CR_TEST(linear_rgb_to_xyz_xyy_yuv_lch_matches_oracle)
{
    const auto rgb = floats(oracle::linear_f32_inputs);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "CIE XYZ float", rgb), oracle::linear_f32_to_xyz_f32,
                           cr_context.out()) == 0);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "CIE xyY float", rgb), oracle::linear_f32_to_xyy_f32,
                           cr_context.out()) == 0);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "CIE Yuv float", rgb), oracle::linear_f32_to_yuv_f32,
                           cr_context.out()) == 0);
    CR_CHECK(bitMismatches(convert<float>("RGB float", "CIE LCH(ab) float", rgb), oracle::linear_f32_to_lch_f32,
                           cr_context.out()) == 0);
}

CR_TEST(cie_lab_to_srgb_u8_matches_oracle)
{
    const auto lab = floats(oracle::lab_f32_inputs);
    CR_CHECK(codeMismatches(convert<std::uint8_t>("CIE Lab float", "R'G'B' u8", lab), oracle::lab_f32_to_srgb_u8,
                            cr_context.out()) == 0);
}

CR_TEST(lab_integer_encodings_are_exact_at_every_threshold)
{
    // binary32 values get the exact rounding of the value itself.
    CR_CHECK(encodingMismatches<std::uint8_t>("CIE Lab u8", 255.0, cr_context.out()) == 0);
    CR_CHECK(encodingMismatches<std::uint16_t>("CIE Lab u16", 65535.0, cr_context.out()) == 0);
    // double values get the defined rounding of (v - lo) * max in double.
    CR_CHECK(doubleEncodingMismatches<std::uint8_t>("CIE Lab u8", 255.0, cr_context.out()) == 0);
    CR_CHECK(doubleEncodingMismatches<std::uint16_t>("CIE Lab u16", 65535.0, cr_context.out()) == 0);
    // Here 255 v is just below 50, so the quotient by 100 is below 1/2, but
    // dividing and adding 1/2 in double rounds up to 1: the correction decides.
    const auto low = convert<std::uint8_t>("CIE Lab double", "CIE Lab u8", std::vector<double>{0x1.9191919191918p-3, 0, 0});
    CR_CHECK(low.size() == 3 && low[0] == 0);
}

CR_TEST(lab_integer_decodings_are_correctly_rounded)
{
    // Every code decodes to a double nearest to lo + c (hi - lo) / max.
    std::vector<std::uint8_t> codes8;
    for (int c = 0; c < 256; ++c) {
        codes8.insert(codes8.end(), 3, static_cast<std::uint8_t>(c));
    }
    std::vector<std::uint16_t> codes16;
    for (int c = 0; c < 65536; ++c) {
        codes16.insert(codes16.end(), 3, static_cast<std::uint16_t>(c));
    }
    const auto lab8 = convert<double>("CIE Lab u8", "CIE Lab double", codes8);
    const auto lab16 = convert<double>("CIE Lab u16", "CIE Lab double", codes16);
    CR_CHECK(lab8.size() == codes8.size() && lab16.size() == codes16.size());
    int wrong = 0;
    for (std::size_t c = 0; c < lab8.size() / 3; ++c) {
        wrong += !nearestQuotient(lab8[c * 3], c * 100.0, 255.0);
        wrong += !nearestQuotient(lab8[c * 3 + 1], c * 255.0 - 128.0 * 255.0, 255.0);
        wrong += !nearestQuotient(lab8[c * 3 + 2], c * 255.0 - 128.0 * 255.0, 255.0);
    }
    for (std::size_t c = 0; c < lab16.size() / 3; ++c) {
        wrong += !nearestQuotient(lab16[c * 3], c * 100.0, 65535.0);
        wrong += !nearestQuotient(lab16[c * 3 + 1], c * 255.0 - 128.0 * 65535.0, 65535.0);
        wrong += !nearestQuotient(lab16[c * 3 + 2], c * 255.0 - 128.0 * 65535.0, 65535.0);
    }
    CR_CHECK(wrong == 0);
}

CR_TEST(lab_integer_decodings_and_round_trips)
{
    // u8: L = 100 c / 255, a and b = c - 128; u16: a and b = 255 c / 65535 - 128.
    std::vector<std::uint8_t> codes8;
    for (int c = 0; c < 256; ++c) {
        codes8.insert(codes8.end(), {static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c)});
    }
    const auto lab8 = convert<double>("CIE Lab u8", "CIE Lab double", codes8);
    int wrong = 0;
    for (int c = 0; c < 256; ++c) {
        wrong += lab8[c * 3] != c * 100.0 / 255.0;
        wrong += lab8[c * 3 + 1] != c - 128.0 || lab8[c * 3 + 2] != c - 128.0;
    }
    CR_CHECK(wrong == 0);
    CR_CHECK(convert<std::uint8_t>("CIE Lab double", "CIE Lab u8", lab8) == codes8);

    std::vector<std::uint16_t> codes16;
    for (int c = 0; c < 65536; ++c) {
        codes16.insert(codes16.end(), {static_cast<std::uint16_t>(c), static_cast<std::uint16_t>(c), static_cast<std::uint16_t>(c)});
    }
    const auto lab16 = convert<double>("CIE Lab u16", "CIE Lab double", codes16);
    CR_CHECK(lab16[32896 * 3 + 1] == 0.0 && lab16[65535 * 3] == 100.0 && lab16[65535 * 3 + 2] == 127.0);
    CR_CHECK(lab16[0] == 0.0 && lab16[1] == -128.0);
    CR_CHECK(convert<std::uint16_t>("CIE Lab double", "CIE Lab u16", lab16) == codes16);

    // Within one model each component keeps its own type: L 50 -> 128, a = b = 0 -> 128.
    const auto mid = convert<std::uint8_t>("CIE Lab float", "CIE Lab u8", std::vector<float>{50.0f, 0.0f, 0.0f});
    CR_CHECK((mid == std::vector<std::uint8_t>{128, 128, 128}));
    // NaN gives the lowest code, out-of-range values clamp.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto clamped = convert<std::uint8_t>("CIE Lab float", "CIE Lab u8",
                                               std::vector<float>{nan, -500.0f, 500.0f, 150.0f, nan, -1e30f});
    CR_CHECK((clamped == std::vector<std::uint8_t>{0, 0, 255, 255, 0, 0}));
}

CR_TEST(hue_rule_and_lab_lch_round_trips)
{
    using namespace chromaroute::detail;
    CR_CHECK(lchFromLab({50, 0, 0})[2] == 0.0 && lchFromLab({50, 0, 0})[1] == 0.0);
    // Chroma exactly 0 gives hue +0 whatever the signs of a and b; atan2 of
    // signed zeros alone gives +-180 or -0.
    for (const Vector3& lab : {Vector3{50, -0.0, -0.0}, Vector3{50, -0.0, 0.0}, Vector3{50, 0.0, -0.0}}) {
        CR_CHECK(lchFromLab(lab)[2] == 0.0 && !std::signbit(lchFromLab(lab)[2]));
    }
    const auto zero_hue =
        convert<float>("CIE Lab float", "CIE LCH(ab) float", std::vector<float>{50.0f, -0.0f, -0.0f, 50.0f, -0.0f, 0.0f});
    CR_CHECK(zero_hue.size() == 6 && zero_hue[2] == 0.0f && !std::signbit(zero_hue[2]) && zero_hue[5] == 0.0f &&
             !std::signbit(zero_hue[5]));
    CR_CHECK(lchFromLab({50, 1, -0.0})[2] == 0.0 && !std::signbit(lchFromLab({50, 1, -0.0})[2]));
    const double tiny_negative = lchFromLab({50, 1, -1e-300})[2];
    CR_CHECK(tiny_negative >= 0.0 && tiny_negative < 360.0);
    CR_CHECK(lchFromLab({50, -1, 0})[2] == 180.0);
    CR_CHECK(std::isnan(lchFromLab({50, std::numeric_limits<double>::quiet_NaN(), 0})[2]));

    // Lab <-> LCH directly, without an XYZ round trip.
    std::vector<double> lab;
    for (int i = 0; i < 1000; ++i) {
        lab.insert(lab.end(), {i * 0.1, std::sin(i * 0.37) * 90.0, std::cos(i * 0.53) * 90.0});
    }
    const auto back = convert<double>("CIE LCH(ab) double", "CIE Lab double", convert<double>("CIE Lab double", "CIE LCH(ab) double", lab));
    double worst = 0;
    for (std::size_t i = 0; i < lab.size(); ++i) {
        worst = std::max(worst, std::fabs(back[i] - lab[i]));
    }
    CR_CHECK(worst < 1e-12);
    // L passes through unchanged.
    const auto lch = convert<double>("CIE Lab double", "CIE LCH(ab) double", lab);
    int l_changed = 0;
    for (std::size_t i = 0; i < lab.size(); i += 3) {
        l_changed += lch[i] != lab[i];
    }
    CR_CHECK(l_changed == 0);

    // LCH -> Lab reduces the hue exactly first: 90, 180 and 270 give exact
    // zeros, and equal angles give identical results however they are written.
    const auto lab_of = [](double hue) { return labFromLch({50, 10, hue}); };
    CR_CHECK(lab_of(90)[1] == 0.0 && lab_of(90)[2] == 10.0);
    CR_CHECK(lab_of(180)[1] == -10.0 && lab_of(180)[2] == 0.0);
    CR_CHECK(lab_of(270)[1] == 0.0 && lab_of(270)[2] == -10.0);
    int differ = 0;
    for (const auto& [hue, same] : {std::pair{360.0, 0.0}, std::pair{3600.0, 0.0}, std::pair{-90.0, 270.0},
                                    std::pair{1e12, 280.0}, std::pair{1e15, 280.0}, std::pair{-1e15, 80.0}}) {
        differ += bitsDiffer(lab_of(hue)[1], lab_of(same)[1]) + bitsDiffer(lab_of(hue)[2], lab_of(same)[2]);
    }
    CR_CHECK(differ == 0);
    CR_CHECK(std::isnan(lab_of(std::numeric_limits<double>::infinity())[1]));
}

CR_TEST(float_lch_stores_the_hue_rules)
{
    // A hue that binary32 rounds up to 360 is stored as 0, and a chroma that
    // rounds to 0 is stored with hue 0.
    const auto lch = convert<float>("CIE Lab float", "CIE LCH(ab) float", std::vector<float>{50.0f, 1.0f, -1e-7f});
    CR_CHECK(lch.size() == 3 && lch[1] == 1.0f && lch[2] == 0.0f);
    // Storing LCH double as float applies the same rules, so the direct
    // conversion equals the one staged through LCH double.
    const auto staged = convert<float>("CIE LCH(ab) double", "CIE LCH(ab) float",
                                       convert<double>("CIE Lab float", "CIE LCH(ab) double", std::vector<float>{50.0f, 1.0f, -1e-7f}));
    CR_CHECK(staged == lch);
    const auto tiny = convert<float>("CIE Lab double", "CIE LCH(ab) alpha float", std::vector<double>{50.0, 1e-300, 1e-300});
    CR_CHECK(tiny.size() == 4 && tiny[1] == 0.0f && tiny[2] == 0.0f && tiny[3] == 1.0f);
    // Colors whose hue rounded up to 360 (sRGB and ProPhoto).
    const auto srgb = convert<float>("R'G'B' u8", "CIE LCH(ab) float", std::vector<std::uint8_t>{174, 48, 101});
    const auto prophoto = convert<float>("R'G'B' u8-ProPhoto", "CIE LCH(ab) float", std::vector<std::uint8_t>{121, 17, 63});
    CR_CHECK(srgb.size() == 3 && srgb[2] >= 0.0f && srgb[2] < 360.0f);
    CR_CHECK(prophoto.size() == 3 && prophoto[2] >= 0.0f && prophoto[2] < 360.0f);
    // A double destination keeps the computed hue.
    const auto exact = convert<double>("CIE Lab double", "CIE LCH(ab) double", std::vector<double>{50.0, 1e-300, 1e-300});
    CR_CHECK(exact.size() == 3 && exact[1] > 0.0 && std::fabs(exact[2] - 45.0) < 1e-12);
}

CR_TEST(near_black_lab_matches_oracle)
{
    // L, a and b are computed without cancelling against the 16/116 offset, so
    // dark colors keep their precision and a dark color is not black.
    CR_CHECK(bitMismatches(convert<float>("RGB float", "CIE Lab float", floats(oracle::dark_f32_inputs)),
                           oracle::dark_f32_to_lab_f32, cr_context.out()) == 0);
    CR_CHECK(bitMismatches(convert<float>("Y float", "CIE L float", floats(oracle::dark_y_f32_inputs)),
                           oracle::dark_y_f32_to_l_f32, cr_context.out()) == 0);
    CR_CHECK(convert<float>("Y float", "CIE L float", std::vector<float>{1e-18f})[0] > 0.0f);
}

CR_TEST(inverse_directions_match_oracle)
{
    std::FILE* out = cr_context.out();
    CR_CHECK(bitMismatches(convert<float>("CIE LCH(ab) float", "CIE Lab float", floats(oracle::lch_f32_inputs)),
                           oracle::lch_f32_to_lab_f32, out) == 0);
    CR_CHECK(bitMismatches(convert<float>("CIE xyY float", "CIE XYZ float", floats(oracle::xyy_f32_inputs)),
                           oracle::xyy_f32_to_xyz_f32, out) == 0);
    CR_CHECK(bitMismatches(convert<float>("CIE Yuv float", "CIE XYZ float", floats(oracle::yuv_f32_inputs)),
                           oracle::yuv_f32_to_xyz_f32, out) == 0);
    const auto lab = floats(oracle::lab_inverse_f32_inputs);
    CR_CHECK(bitMismatches(convert<float>("CIE Lab float", "CIE XYZ float", lab), oracle::lab_inverse_f32_to_xyz_f32,
                           out) == 0);
    // CIE -> RGB uses the destination space; the CIE format's own space plays no part.
    const auto prophoto = convert<std::uint8_t>("CIE Lab float", "R'G'B' u8-ProPhoto", lab);
    CR_CHECK(codeMismatches(prophoto, oracle::lab_inverse_f32_to_prophoto_u8, out) == 0);
    CR_CHECK(convert<std::uint8_t>("CIE Lab float-Rec2020", "R'G'B' u8-ProPhoto", lab) == prophoto);
    // CIE -> gray is Y itself, whatever the chromaticity.
    CR_CHECK(codeMismatches(convert<std::uint8_t>("CIE Lab float", "Y' u8", lab), oracle::lab_inverse_f32_to_gray_u8,
                            out) == 0);
    const auto y = convert<double>("CIE XYZ double", "Y double",
                                   std::vector<double>{0.3, 0.6, 0.1, 2.0, -0.5, 7.0, 0.0, 1e-300, -3.0});
    CR_CHECK((y == std::vector<double>{0.6, -0.5, 1e-300}));
}

CR_TEST(xyz_family_conversions_skip_the_white_normalization)
{
    // Without a Lab or LCH side the hub is XYZ itself: RGB -> XYZ is the space's
    // matrix, and XYZ <-> xyY <-> Yuv apply only their own formulas.
    std::vector<double> rgb;
    for (int i = 0; i < 300; ++i) {
        rgb.insert(rgb.end(), {std::sin(i * 0.7) * 0.6 + 0.4, std::cos(i * 1.3) * 0.6 + 0.4,
                               std::sin(i * 2.9 + 1.0) * 0.6 + 0.4});
    }
    const auto& m = PixelFormat::fromName("RGB double")->space().rgbToXyz();
    const auto xyz = convert<double>("RGB double", "CIE XYZ double", rgb);
    const auto xyy = convert<double>("CIE XYZ double", "CIE xyY double", xyz);
    const auto yuv = convert<double>("CIE XYZ double", "CIE Yuv double", xyz);
    const auto from_xyy = convert<double>("CIE xyY double", "CIE XYZ double", xyy);
    CR_CHECK(xyz.size() == rgb.size() && xyy.size() == rgb.size() && yuv.size() == rgb.size() &&
             from_xyy.size() == rgb.size());
    int differ = 0;
    for (std::size_t p = 0; p < xyz.size() / 3; ++p) {
        const double* c = &rgb[p * 3];
        const double* v = &xyz[p * 3];
        for (std::size_t r = 0; r < 3; ++r) {
            differ += bitsDiffer(v[r], m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2]);
        }
        const double sum = v[0] + v[1] + v[2];
        differ += bitsDiffer(xyy[p * 3], v[0] / sum) + bitsDiffer(xyy[p * 3 + 1], v[1] / sum);
        const double denominator = v[0] + 15.0 * v[1] + 3.0 * v[2];
        differ += bitsDiffer(yuv[p * 3 + 1], 4.0 * v[0] / denominator) + bitsDiffer(yuv[p * 3 + 2], 9.0 * v[1] / denominator);
        const double* q = &xyy[p * 3];
        differ += bitsDiffer(from_xyy[p * 3], q[0] * q[2] / q[1]);
    }
    CR_CHECK(differ == 0);
}

CR_TEST(chromaticity_of_black_and_degenerate_inputs)
{
    using namespace chromaroute::detail;
    const auto black_xyy = convert<double>("CIE XYZ double", "CIE xyY double", std::vector<double>{0, 0, 0});
    CR_CHECK(black_xyy[0] == white_x && black_xyy[1] == white_y && black_xyy[2] == 0.0);
    const auto black_yuv = convert<double>("CIE XYZ double", "CIE Yuv double", std::vector<double>{0, 0, 0});
    CR_CHECK(black_yuv[0] == 0.0 && black_yuv[1] == white_u && black_yuv[2] == white_v);
    // The white's chromaticity is the ICC D50 white's (exact rationals: 0.345702922, 0.358537533;
    // u' v' 0.209166899, 0.488098026).
    CR_CHECK(std::fabs(white_x - 0.34570292198022834) < 1e-15 && std::fabs(white_y - 0.3585375327567059) < 1e-15);
    CR_CHECK(std::fabs(white_u - 0.2091668990651375) < 1e-15 && std::fabs(white_v - 0.4880980260887627) < 1e-15);
    // y = 0 (or v' = 0) is black when Y = 0 and undefined otherwise.
    const auto xyz = convert<double>("CIE xyY double", "CIE XYZ double", std::vector<double>{0.3, 0.0, 0.0, 0.3, 0.0, 1.0});
    CR_CHECK(xyz[0] == 0.0 && xyz[2] == 0.0 && std::isnan(xyz[3]) && xyz[4] == 1.0 && std::isnan(xyz[5]));
    const auto from_yuv = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{0.0, 0.2, 0.0, 1.0, 0.2, 0.0});
    CR_CHECK(from_yuv[0] == 0.0 && from_yuv[2] == 0.0 && std::isnan(from_yuv[3]));
    // Only black takes the white's chromaticity, signed zeros included. Another
    // color whose sum (or u'v' denominator) is 0 has none: NaN.
    const auto signed_black = convert<double>("CIE XYZ double", "CIE xyY double", std::vector<double>{-0.0, 0.0, -0.0});
    CR_CHECK(signed_black[0] == white_x && signed_black[1] == white_y && signed_black[2] == 0.0);
    const auto no_xy = convert<double>("CIE XYZ double", "CIE xyY double", std::vector<double>{1.0, -0.5, -0.5});
    CR_CHECK(std::isnan(no_xy[0]) && std::isnan(no_xy[1]) && no_xy[2] == -0.5);
    const auto no_uv = convert<double>("CIE XYZ double", "CIE Yuv double", std::vector<double>{15.0, -1.0, 0.0});
    CR_CHECK(no_uv[0] == -1.0 && std::isnan(no_uv[1]) && std::isnan(no_uv[2]));
    // A NaN chromaticity of black stays NaN.
    const double undefined = std::numeric_limits<double>::quiet_NaN();
    const auto nan_xy = convert<double>("CIE xyY double", "CIE XYZ double", std::vector<double>{undefined, 0.0, 0.0});
    CR_CHECK(std::isnan(nan_xy[0]) && nan_xy[1] == 0.0 && std::isnan(nan_xy[2]));
    const auto nan_uv = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{0.0, undefined, 0.0});
    CR_CHECK(std::isnan(nan_uv[0]) && nan_uv[1] == 0.0 && std::isnan(nan_uv[2]));
    // The white round-trips through xyY and Yuv.
    const auto white = std::vector<double>{cie_white[0], cie_white[1], cie_white[2]};
    for (const char* model : {"CIE xyY double", "CIE Yuv double", "CIE Lab double", "CIE LCH(ab) double"}) {
        const auto back = convert<double>(model, "CIE XYZ double", convert<double>("CIE XYZ double", model, white));
        CR_CHECK(std::fabs(back[0] - white[0]) < 1e-15 && std::fabs(back[1] - 1.0) < 1e-15 &&
                 std::fabs(back[2] - white[2]) < 1e-15);
    }
    // NaN propagates.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto lab = convert<double>("RGB double", "CIE Lab double", std::vector<double>{nan, 0.5, 0.5});
    CR_CHECK(std::isnan(lab[0]) || std::isnan(lab[1]));
}

CR_TEST(cie_alpha_partial_formats_and_round_trips)
{
    // Alpha is straight and copied; models without alpha read as alpha 1.
    const auto lab_alpha = convert<float>("RGBA float", "CIE Lab alpha float", std::vector<float>{0.2f, 0.4f, 0.6f, 0.25f});
    CR_CHECK(lab_alpha.size() == 4 && lab_alpha[3] == 0.25f);
    const auto rgba = convert<float>("CIE Lab float", "RGBA float", std::vector<float>{50.0f, 10.0f, -10.0f});
    CR_CHECK(rgba.size() == 4 && rgba[3] == 1.0f);
    // Associated sources are un-associated first.
    const auto straight = convert<double>("RGBA double", "CIE Lab alpha double", std::vector<double>{0.2, 0.4, 0.6, 0.5});
    const auto premultiplied = convert<double>("RaGaBaA double", "CIE Lab alpha double", std::vector<double>{0.1, 0.2, 0.3, 0.5});
    CR_CHECK(straight == premultiplied);

    // CIE L float stores L only: it reads as a neutral gray, and writes only L.
    const auto gray = convert<double>("CIE L float", "RGB double", std::vector<float>{50.0f});
    CR_CHECK(gray.size() == 3 && std::fabs(gray[0] - gray[1]) < 1e-15 && std::fabs(gray[1] - gray[2]) < 1e-15);
    const auto l_only = convert<float>("CIE Lab float", "CIE L float", std::vector<float>{42.0f, 30.0f, -20.0f});
    CR_CHECK((l_only == std::vector<float>{42.0f}));
    const auto l_alpha = convert<float>("CIE L alpha float", "CIE Lab alpha float", std::vector<float>{42.0f, 0.75f});
    CR_CHECK((l_alpha == std::vector<float>{42.0f, 0.0f, 0.0f, 0.75f}));
    const auto from_y = convert<float>("Y float", "CIE L float", std::vector<float>{1.0f, 0.0f});
    CR_CHECK(from_y.size() == 2 && std::fabs(from_y[0] - 100.0f) < 1e-4f && from_y[1] == 0.0f);
    // A format with its own layout keeps it in another space.
    const std::vector<std::uint8_t> codes{128, 128, 128, 0, 255, 17};
    CR_CHECK(convert<double>("CIE Lab u8-Rec2020", "CIE Lab double", codes) ==
             convert<double>("CIE Lab u8", "CIE Lab double", codes));

    // Every CIE model round-trips linear RGB in double.
    std::vector<double> rgb;
    for (int i = 0; i < 300; ++i) {
        rgb.insert(rgb.end(), {(i % 10) / 9.0, ((i / 10) % 10) / 9.0, (i % 7) / 6.0 + 0.01});
    }
    for (const char* model : {"CIE XYZ double", "CIE xyY double", "CIE Yuv double", "CIE Lab double", "CIE LCH(ab) double"}) {
        const auto back = convert<double>(model, "RGB double", convert<double>("RGB double", model, rgb));
        double worst = 0;
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            worst = std::max(worst, std::fabs(back[i] - rgb[i]));
        }
        CR_CHECK(worst < 1e-13);
        if (worst >= 1e-13) {
            std::fprintf(cr_context.out(), "  %s round trip error %g\n", model, worst);
        }
    }
}

CR_TEST(cie_alpha_to_associated_rgb)
{
    // Colors are multiplied by the used alpha after encoding, as for any
    // straight source; alpha below the floor, 0 and negative included.
    using chromaroute::detail::usedAlpha;
    const std::vector<double> lab{50, 20, -20, 0.5, 60, -30, 10, 1e-6, 40, 5, 5, 0.0, 70, 10, 30, -0.25, 30, 0, 0, 1.0};
    for (const auto& [straight_name, associated_name] :
         {std::pair{"RGBA double", "RaGaBaA double"}, std::pair{"R'G'B'A double", "R'aG'aB'aA double"}}) {
        const auto straight = convert<double>("CIE Lab alpha double", straight_name, lab);
        const auto associated = convert<double>("CIE Lab alpha double", associated_name, lab);
        int differ = associated.size() != lab.size() || straight.size() != lab.size();
        for (std::size_t p = 0; differ == 0 && p < lab.size() / 4; ++p) {
            const double alpha = straight[p * 4 + 3];
            for (std::size_t i = 0; i < 3; ++i) {
                differ += bitsDiffer(associated[p * 4 + i], straight[p * 4 + i] * usedAlpha(alpha));
            }
            differ += bitsDiffer(associated[p * 4 + 3], alpha) + bitsDiffer(alpha, lab[p * 4 + 3]);
        }
        CR_CHECK(differ == 0);
    }
}

CR_TEST(l_only_sources_fill_every_pixel)
{
    // 300 pixels cross the 128-pixel chunks. A chromatic conversion before each
    // run leaves nonzero values in the scratch buffer, which the fill must
    // overwrite for every pixel.
    std::vector<float> chromatic, l_only, l_alpha;
    for (int i = 0; i < 300; ++i) {
        chromatic.insert(chromatic.end(), {40.0f, 30.0f + static_cast<float>(i % 7), -25.0f});
        l_only.push_back(static_cast<float>(i % 100));
        l_alpha.insert(l_alpha.end(), {static_cast<float>(i % 100), static_cast<float>(i) / 300.0f});
    }
    (void)convert<double>("CIE Lab float", "RGB double", chromatic);
    const auto lab = convert<float>("CIE L float", "CIE Lab float", l_only);
    (void)convert<double>("CIE Lab float", "RGB double", chromatic);
    const auto lab_alpha = convert<float>("CIE L alpha float", "CIE Lab alpha float", l_alpha);
    (void)convert<double>("CIE Lab float", "RGB double", chromatic);
    const auto rgb = convert<double>("CIE L float", "RGB double", l_only);
    int wrong = lab.size() != 900 || lab_alpha.size() != 1200 || rgb.size() != 900;
    for (std::size_t p = 0; wrong == 0 && p < 300; ++p) {
        wrong += lab[p * 3] != l_only[p] || lab[p * 3 + 1] != 0.0f || lab[p * 3 + 2] != 0.0f;
        wrong += lab_alpha[p * 4] != l_alpha[p * 2] || lab_alpha[p * 4 + 1] != 0.0f || lab_alpha[p * 4 + 2] != 0.0f ||
                 lab_alpha[p * 4 + 3] != l_alpha[p * 2 + 1];
        wrong += std::fabs(rgb[p * 3] - rgb[p * 3 + 1]) > 1e-12 || std::fabs(rgb[p * 3 + 1] - rgb[p * 3 + 2]) > 1e-12;
    }
    CR_CHECK(wrong == 0);
}

CR_TEST(chromaticities_keep_finite_results_at_every_exponent)
{
    // Scaling XYZ by a power of two keeps x, y, u' and v' bit for bit, also where
    // the sum X + Y + Z would overflow.
    std::vector<double> base;
    for (int i = 0; i < 64; ++i) {
        base.insert(base.end(), {0.5 + 0.4 * std::sin(i * 0.9), 0.5 + 0.45 * std::cos(i * 1.7), 0.5 + 0.3 * std::sin(i * 2.3)});
    }
    const auto xyy = convert<double>("CIE XYZ double", "CIE xyY double", base);
    const auto yuv = convert<double>("CIE XYZ double", "CIE Yuv double", base);
    int differ = 0;
    for (int k : {-1000, -600, -60, 60, 600, 1000, 1023}) { // inputs stay normal
        std::vector<double> scaled;
        for (double v : base) {
            scaled.push_back(std::ldexp(v, k));
        }
        const auto xyy_k = convert<double>("CIE XYZ double", "CIE xyY double", scaled);
        const auto yuv_k = convert<double>("CIE XYZ double", "CIE Yuv double", scaled);
        for (std::size_t p = 0; p < base.size() / 3; ++p) {
            differ += bitsDiffer(xyy_k[p * 3], xyy[p * 3]) + bitsDiffer(xyy_k[p * 3 + 1], xyy[p * 3 + 1]);
            differ += bitsDiffer(yuv_k[p * 3 + 1], yuv[p * 3 + 1]) + bitsDiffer(yuv_k[p * 3 + 2], yuv[p * 3 + 2]);
            differ += bitsDiffer(xyy_k[p * 3 + 2], scaled[p * 3 + 1]) + bitsDiffer(yuv_k[p * 3], scaled[p * 3 + 1]);
        }
    }
    CR_CHECK(differ == 0);
    const auto big = convert<double>("CIE XYZ double", "CIE xyY double", std::vector<double>{1e308, 1e308, 1e308});
    CR_CHECK(std::fabs(big[0] - 1.0 / 3.0) < 1e-15 && big[0] == big[1] && big[2] == 1e308);
    const auto big_uv = convert<double>("CIE XYZ double", "CIE Yuv double", std::vector<double>{1e308, 1e308, 1e308});
    CR_CHECK(big_uv[0] == 1e308 && std::fabs(big_uv[1] - 4.0 / 19.0) < 1e-15 && std::fabs(big_uv[2] - 9.0 / 19.0) < 1e-15);

    // The inverses combine mantissas before exponents, so a representable result
    // survives a product that would overflow or underflow on its own.
    const double tiny = std::numeric_limits<double>::denorm_min();
    const auto from_tiny = convert<double>("CIE xyY double", "CIE XYZ double", std::vector<double>{0.5, tiny, tiny});
    CR_CHECK(from_tiny[0] == 0.5 && from_tiny[1] == tiny && from_tiny[2] == 0.5);
    const auto from_huge = convert<double>("CIE xyY double", "CIE XYZ double", std::vector<double>{4.0, 8.0, 1e308});
    CR_CHECK(from_huge[0] == 1e308 / 2 && from_huge[2] == -1.375 * 1e308);
    // 9 u' Y overflows; X = 9 u' Y / (4 v') = 1.227e308 does not.
    const auto uv_huge = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{1e308, 0.3, 0.55});
    CR_CHECK(std::fabs(uv_huge[0] / (2.7 / 2.2 * 1e308) - 1.0) < 1e-15 && uv_huge[1] == 1e308 && std::isfinite(uv_huge[2]));
    const auto uv_tiny = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{tiny, 0.2, tiny});
    CR_CHECK(uv_tiny[0] == 2.25 * 0.2 && std::fabs(uv_tiny[2] - 2.85) < 1e-15);
    // A constant factor is applied before the exponent too: Z = Y (12 - 3 u' -
    // 20 v') / (4 v') = 7e307 although Y (12 - 3 u' - 20 v') / v' overflows.
    const auto uv_factor = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{1e308, 0.2, 0.5});
    CR_CHECK(std::fabs(uv_factor[0] / 9e307 - 1.0) < 1e-15 && std::fabs(uv_factor[2] / 7e307 - 1.0) < 1e-15);
    // Near underflow every inverse result is rounded once: it equals the same
    // conversion at a normal scale, scaled down.
    differ = 0;
    for (double y : {1e-308, 3.3e-310, 1e-311, 7e-313, 2.5e-315, 1e-317, 4e-319, 1.7e-320, 6e-322}) {
        for (const auto& [a, b] : {std::pair{0.2, 0.5}, std::pair{0.19, 0.47}, std::pair{0.41, 0.33}}) {
            const auto small_uv = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{y, a, b});
            const auto large_uv = convert<double>("CIE Yuv double", "CIE XYZ double", std::vector<double>{std::ldexp(y, 200), a, b});
            const auto small_xy = convert<double>("CIE xyY double", "CIE XYZ double", std::vector<double>{a, b, y});
            const auto large_xy = convert<double>("CIE xyY double", "CIE XYZ double", std::vector<double>{a, b, std::ldexp(y, 200)});
            for (std::size_t i : {std::size_t{0}, std::size_t{2}}) {
                differ += bitsDiffer(small_uv[i], std::ldexp(large_uv[i], -200));
                differ += bitsDiffer(small_xy[i], std::ldexp(large_xy[i], -200));
            }
        }
    }
    CR_CHECK(differ == 0);
    // The inverse scales exactly with Y.
    differ = 0;
    for (int k : {-1000, -100, 100, 1000}) {
        std::vector<double> scaled = xyy;
        for (std::size_t p = 0; p < scaled.size() / 3; ++p) {
            scaled[p * 3 + 2] = std::ldexp(scaled[p * 3 + 2], k);
        }
        const auto plain = convert<double>("CIE xyY double", "CIE XYZ double", xyy);
        const auto back = convert<double>("CIE xyY double", "CIE XYZ double", scaled);
        for (std::size_t i = 0; i < plain.size(); ++i) {
            differ += bitsDiffer(back[i], std::ldexp(plain[i], k));
        }
    }
    CR_CHECK(differ == 0);
    // A non-finite XYZ has no chromaticity.
    const double inf = std::numeric_limits<double>::infinity();
    const auto infinite = convert<double>("CIE XYZ double", "CIE xyY double", std::vector<double>{inf, 1.0, 1.0});
    CR_CHECK(std::isnan(infinite[0]) && std::isnan(infinite[1]) && infinite[2] == 1.0);
    const auto infinite_uv = convert<double>("CIE XYZ double", "CIE Yuv double", std::vector<double>{1.0, 1.0, inf});
    CR_CHECK(infinite_uv[0] == 1.0 && std::isnan(infinite_uv[1]) && std::isnan(infinite_uv[2]));
}

CR_TEST(matrix_spaces_use_their_own_matrix_for_neutrals)
{
    // A space from fromRgbToXyz keeps the caller's matrix for every pixel (N9).
    // This rounded profile-style matrix maps R = G = B to (0.9642, 1, 0.8251),
    // not exactly to the D50 white.
    using chromaroute::ColorSpace;
    using chromaroute::TransferFunction;
    using chromaroute::detail::Vector3;
    const chromaroute::Matrix3 m{{{0.4360, 0.3851, 0.1431}, {0.2225, 0.7169, 0.0606}, {0.0139, 0.0971, 0.7141}}};
    const TransferFunction linear = TransferFunction::linear();
    const ColorSpace space = *ColorSpace::fromRgbToXyz("review-F1", {0.9642, 1.0, 0.8251}, m, linear, linear, linear);
    const std::string in_space = "-" + std::string(space.name());
    const auto name = [&](const char* format) { return std::string(format) + in_space; };
    const auto product = [&](const double* c) {
        Vector3 v{};
        for (std::size_t r = 0; r < 3; ++r) {
            v[r] = m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2];
        }
        return v;
    };

    // Neutrals, gray and associated alpha all go through the matrix; so does
    // the neighbor of a neutral, so there is no step at R = G = B.
    const std::vector<double> rgb{1, 1, 1, 0.5, 0.5, 0.5, 1, 1, std::nextafter(1.0, 2.0)};
    const auto xyz = convert<double>(name("RGB double").c_str(), "CIE XYZ double", rgb);
    const auto from_gray = convert<double>(name("Y double").c_str(), "CIE XYZ double", std::vector<double>{1.0, 0.5});
    const auto from_associated =
        convert<double>(name("RaGaBaA double").c_str(), "CIE XYZ alpha double", std::vector<double>{0.5, 0.5, 0.5, 0.5});
    int differ = xyz.size() != 9 || from_gray.size() != 6 || from_associated.size() != 4;
    for (std::size_t p = 0; differ == 0 && p < 3; ++p) {
        const Vector3 expected = product(&rgb[p * 3]);
        for (std::size_t r = 0; r < 3; ++r) {
            differ += bitsDiffer(xyz[p * 3 + r], expected[r]);
            differ += p < 2 && bitsDiffer(from_gray[p * 3 + r], expected[r]);
            differ += p == 0 && bitsDiffer(from_associated[r], expected[r]);
        }
    }
    CR_CHECK(differ == 0);

    // Gray converts exactly like the RGB neutral to RGB and gray, in this space's
    // own terms within it and through the matrices to other spaces.
    differ = 0;
    for (const std::string& to : {std::string("RGB double"), std::string("Y double"), std::string("RGB double-ProPhoto"),
                                  std::string("Y double-Rec2020"), name("RGB double")}) {
        const auto as_gray = convert<double>(name("Y double").c_str(), to.c_str(), std::vector<double>{1.0, 0.5});
        const auto as_rgb = convert<double>(name("RGB double").c_str(), to.c_str(), std::vector<double>{1, 1, 1, 0.5, 0.5, 0.5});
        differ += as_gray.size() != as_rgb.size() || std::memcmp(as_gray.data(), as_rgb.data(), as_gray.size() * 8) != 0;
    }
    CR_CHECK(differ == 0);

    // Round trips through each CIE model return the neutrals.
    double worst = 0;
    for (const char* model : {"CIE XYZ double", "CIE xyY double", "CIE Yuv double", "CIE Lab double", "CIE LCH(ab) double"}) {
        const auto back = convert<double>(model, name("RGB double").c_str(), convert<double>(name("RGB double").c_str(), model, rgb));
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            worst = std::max(worst, back.size() == rgb.size() ? std::fabs(back[i] - rgb[i]) : 1.0);
        }
    }
    CR_CHECK(worst < 1e-14);
}
