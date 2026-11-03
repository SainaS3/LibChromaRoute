// Parametric and sampled tone curves (Milestone 8a): decode and inverse
// against the oracle (tests/data/curve_oracle_data.hpp), the factories'
// validation and interning, the curves' rules outside [0, 1], and the
// planner's rule that values stay encoded with them only within one model.

#include "support/conversion_checks.hpp"
#include "support/test_harness.hpp"

#include "data/curve_oracle_data.hpp"

#include <chromaroute/chromaroute.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

using chromaroute::ColorSpace;
using chromaroute::ErrorCode;
using chromaroute::ParametricCurve;
using chromaroute::TransferFunction;

namespace oracle = chromaroute::test::oracle;
using namespace chromaroute::test;

namespace {

using Kind = TransferFunction::Kind;

TransferFunction parametricFrom(std::span<const std::uint32_t> definition)
{
    double p[7] = {};
    for (std::size_t i = 1; i < definition.size(); ++i) {
        p[i - 1] = static_cast<std::int32_t>(definition[i]) / 65536.0;
    }
    return *TransferFunction::parametric(
        {static_cast<int>(definition[0]), p[0], p[1], p[2], p[3], p[4], p[5], p[6]});
}

TransferFunction sampledFrom(std::span<const std::uint32_t> entries)
{
    std::vector<double> samples;
    for (std::uint32_t e : entries) {
        samples.push_back(e / 65535.0);
    }
    return *TransferFunction::sampled(samples);
}

// A space with sRGB's primaries and the curve on every channel; R'G'B' to RGB
// in it is the curve alone.
std::string spaceWith(const TransferFunction& curve, const char* name)
{
    return std::string(ColorSpace::fromChromaticities(name, ColorSpace::srgb().chromaticities(), curve)->name());
}

// The oracle's inputs as pixels (three channels), padded with the first input.
std::vector<float> pixels(std::span<const std::uint32_t> bits)
{
    std::vector<float> values = floats(bits);
    while (values.size() % 3 != 0) {
        values.push_back(values.front());
    }
    return values;
}

int curveMismatches(const TransferFunction& curve, const char* name, std::span<const std::uint32_t> decode_in,
                    std::span<const std::uint32_t> decode_out, std::span<const std::uint32_t> encode_in,
                    std::span<const std::uint32_t> encode_out, std::FILE* log)
{
    const std::string space = "-" + spaceWith(curve, name);
    auto decoded = convert<float>(("R'G'B' float" + space).c_str(), ("RGB float" + space).c_str(), pixels(decode_in));
    auto encoded = convert<float>(("RGB float" + space).c_str(), ("R'G'B' float" + space).c_str(), pixels(encode_in));
    decoded.resize(decode_in.size());
    encoded.resize(encode_in.size());
    return bitMismatches(decoded, decode_out, log) + bitMismatches(encoded, encode_out, log);
}

std::vector<double> through(const std::string& from, const std::string& to, const std::vector<double>& values)
{
    return convert<double>(from.c_str(), to.c_str(), values);
}

} // namespace

CR_TEST(parametric_curves_match_oracle)
{
    std::FILE* log = cr_context.out();
    CR_CHECK(curveMismatches(parametricFrom(oracle::curve_para1), "curve-para1", oracle::curve_para1_decode_inputs,
                             oracle::curve_para1_decode_f32, oracle::curve_para1_encode_inputs,
                             oracle::curve_para1_encode_f32, log) == 0);
    CR_CHECK(curveMismatches(parametricFrom(oracle::curve_para2), "curve-para2", oracle::curve_para2_decode_inputs,
                             oracle::curve_para2_decode_f32, oracle::curve_para2_encode_inputs,
                             oracle::curve_para2_encode_f32, log) == 0);
    CR_CHECK(curveMismatches(parametricFrom(oracle::curve_para3), "curve-para3", oracle::curve_para3_decode_inputs,
                             oracle::curve_para3_decode_f32, oracle::curve_para3_encode_inputs,
                             oracle::curve_para3_encode_f32, log) == 0);
    CR_CHECK(curveMismatches(parametricFrom(oracle::curve_para4), "curve-para4", oracle::curve_para4_decode_inputs,
                             oracle::curve_para4_decode_f32, oracle::curve_para4_encode_inputs,
                             oracle::curve_para4_encode_f32, log) == 0);
}

CR_TEST(sampled_curves_match_oracle)
{
    std::FILE* log = cr_context.out();
    CR_CHECK(curveMismatches(sampledFrom(oracle::curve_table0), "curve-table0", oracle::curve_table0_decode_inputs,
                             oracle::curve_table0_decode_f32, oracle::curve_table0_encode_inputs,
                             oracle::curve_table0_encode_f32, log) == 0);
    CR_CHECK(curveMismatches(sampledFrom(oracle::curve_table1), "curve-table1", oracle::curve_table1_decode_inputs,
                             oracle::curve_table1_decode_f32, oracle::curve_table1_encode_inputs,
                             oracle::curve_table1_encode_f32, log) == 0);
    CR_CHECK(curveMismatches(sampledFrom(oracle::curve_table2), "curve-table2", oracle::curve_table2_decode_inputs,
                             oracle::curve_table2_decode_f32, oracle::curve_table2_encode_inputs,
                             oracle::curve_table2_encode_f32, log) == 0);
    CR_CHECK(curveMismatches(sampledFrom(oracle::curve_table3), "curve-table3", oracle::curve_table3_decode_inputs,
                             oracle::curve_table3_decode_f32, oracle::curve_table3_encode_inputs,
                             oracle::curve_table3_encode_f32, log) == 0);
}

CR_TEST(curve_factories_validate_and_intern)
{
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    const ParametricCurve bad[] = {
        {-1, 2.2, 1, 0, 0, 0, 0, 0}, {5, 2.2, 1, 0, 0, 0, 0, 0}, {1, 0.0, 1, 0, 0, 0, 0, 0},
        {1, nan, 1, 0, 0, 0, 0, 0},  {2, 2.2, 0, 0, 0, 0, 0, 0}, {3, 2.4, 1, 0, -0.1, 0.04, 0, 0},
        {4, 2.4, 1, 0, 0.1, 0.04, nan, 0},
    };
    for (const ParametricCurve& c : bad) {
        const auto t = TransferFunction::parametric(c);
        CR_CHECK(!t && t.error().code == ErrorCode::invalid_argument);
    }
    // Unused parameters are ignored, even NaN, and stored as 0; equal curves
    // are one handle, found by name.
    const auto a = TransferFunction::parametric({1, 2.4, 0.9, 0.1, nan, nan, nan, nan});
    const auto b = TransferFunction::parametric({1, 2.4, 0.9, 0.1, 0, 0, 0, 0});
    CR_CHECK(a.has_value() && b.has_value() && *a == *b && a->kind() == Kind::parametric);
    CR_CHECK(a && a->parametricCurve().c == 0.0 && a->parametricCurve().f == 0.0 && a->name().starts_with("para1:"));
    CR_CHECK(a && TransferFunction::fromName(a->name()).has_value() && *TransferFunction::fromName(a->name()) == *a);
    // Type 0 is a gamma.
    const auto type0 = TransferFunction::parametric({0, 2.2, 0, 0, 0, 0, 0, 0});
    CR_CHECK(type0 && type0->kind() == Kind::gamma && *type0 == *TransferFunction::gamma(2.2));

    for (const std::vector<double>& s : {std::vector<double>{}, std::vector<double>{0.5}, std::vector<double>{0, nan},
                                         std::vector<double>(65537, 0.0)}) {
        const auto t = TransferFunction::sampled(s);
        CR_CHECK(!t && t.error().code == ErrorCode::invalid_argument);
    }
    const auto t1 = TransferFunction::sampled(std::vector<double>{0, 0.25, 1});
    const auto t2 = TransferFunction::sampled(std::vector<double>{0, 0.25, 1});
    const auto t3 = TransferFunction::sampled(std::vector<double>{0, 0.26, 1});
    CR_CHECK(t1 && t2 && t3 && *t1 == *t2 && !(*t1 == *t3) && t1->kind() == Kind::sampled);
    CR_CHECK(t1 && t1->samples().size() == 3 && t1->samples()[1] == 0.25 && t1->name().starts_with("table3:"));
    CR_CHECK(t1 && *TransferFunction::fromName(t1->name()) == *t1);
}

CR_TEST(curves_follow_their_rules_outside_the_unit_range)
{
    // Tables: inputs outside [0, 1] take the end samples; the inverse takes
    // the smallest input of a flat run, and in a table that falls somewhere the
    // first segment holding the value.
    const auto flat = *TransferFunction::sampled(std::vector<double>{0, 0, 0.5, 0.5, 1});
    const std::string in_flat = "-" + spaceWith(flat, "curve-flat");
    CR_CHECK(through("R'G'B' double" + in_flat, "RGB double" + in_flat, {-0.5, 1.5, 0.125}) ==
             (std::vector<double>{0, 1, 0}));
    CR_CHECK(through("RGB double" + in_flat, "R'G'B' double" + in_flat, {0.0, 0.5, 2.0}) ==
             (std::vector<double>{0, 0.5, 1}));
    const auto wavy = *TransferFunction::sampled(std::vector<double>{0, 0.6, 0.4, 1});
    const std::string in_wavy = "-" + spaceWith(wavy, "curve-wavy");
    const auto first = through("RGB double" + in_wavy, "R'G'B' double" + in_wavy, {0.5, -1.0, 2.0});
    CR_CHECK(first.size() == 3 && std::fabs(first[0] - 0.5 / 0.6 / 3.0) < 1e-16 && first[1] == 0.0 && first[2] == 1.0);

    // Parametric type 3 is linear below d, also below 0; types 1 and 2 hold
    // their constant below -b/a and invert to it.
    const auto para3 = *TransferFunction::parametric({3, 2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045, 0, 0});
    const std::string in3 = "-" + spaceWith(para3, "curve-p3");
    const auto negative = through("R'G'B' double" + in3, "RGB double" + in3, {-0.1, 0.0, 0.04045});
    CR_CHECK(negative.size() == 3 && negative[0] == (1 / 12.92) * -0.1 && negative[1] == 0.0);
    // The linear piece keeps the sign of zero, as the sRGB curve does.
    const auto zeros = through("R'G'B' double" + in3, "RGB double" + in3, {-0.0, 0.0, -0.0});
    CR_CHECK(zeros.size() == 3 && std::signbit(zeros[0]) && !std::signbit(zeros[1]) && std::signbit(zeros[2]));
    const auto para2 = *TransferFunction::parametric({2, 2.2, 0.95, 0.05, 0.02, 0, 0, 0});
    const std::string in2 = "-" + spaceWith(para2, "curve-p2");
    const auto held = through("R'G'B' double" + in2, "RGB double" + in2, {-0.5, -0.2, 0.0});
    CR_CHECK(held.size() == 3 && held[0] == 0.02 && held[1] == 0.02);
    const auto knee = through("RGB double" + in2, "R'G'B' double" + in2, {0.0, 0.02, 0.01});
    CR_CHECK(knee.size() == 3 && knee[0] == -0.05 / 0.95 && knee[1] == -0.05 / 0.95 && knee[2] == -0.05 / 0.95);

    // NaN propagates through both kinds.
    const auto nan_in = std::vector<double>{std::numeric_limits<double>::quiet_NaN(), 0.5, 0.5};
    CR_CHECK(std::isnan(through("R'G'B' double" + in_flat, "RGB double" + in_flat, nan_in)[0]));
    CR_CHECK(std::isnan(through("RGB double" + in3, "R'G'B' double" + in3, nan_in)[0]));
}

CR_TEST(sampled_curves_invert_plateaus_and_huge_samples)
{
    // Values beyond every sample take the input of the first largest (or
    // smallest) one, also when the table ends on a plateau or is constant.
    const auto plateau = *TransferFunction::sampled(std::vector<double>{0, 1, 1});
    const std::string in_plateau = "-" + spaceWith(plateau, "curve-plateau");
    CR_CHECK(through("RGB double" + in_plateau, "R'G'B' double" + in_plateau, {2.0, 1.0, -1.0}) ==
             (std::vector<double>{0.5, 0.5, 0.0}));
    const auto constant = *TransferFunction::sampled(std::vector<double>{0.5, 0.5, 0.5});
    const std::string in_constant = "-" + spaceWith(constant, "curve-constant");
    CR_CHECK(through("RGB double" + in_constant, "R'G'B' double" + in_constant, {0.2, 0.5, 0.9}) ==
             (std::vector<double>{0, 0, 0}));
    CR_CHECK(through("R'G'B' double" + in_constant, "RGB double" + in_constant, {0.0, 0.7, 2.0}) ==
             (std::vector<double>{0.5, 0.5, 0.5}));

    // Finite samples near the double range neither overflow when interpolated
    // nor when inverted: halfway between -1e308 and 1e308 is 0, and back.
    const auto huge = *TransferFunction::sampled(std::vector<double>{-1e308, 1e308});
    const std::string in_huge = "-" + spaceWith(huge, "curve-huge");
    const auto decoded = through("R'G'B' double" + in_huge, "RGB double" + in_huge, {0.5, 0.25, 1.0});
    CR_CHECK(decoded.size() == 3 && decoded[0] == 0.0 && std::fabs(decoded[1] + 0.5e308) < 1e293 && decoded[2] == 1e308);
    const auto encoded = through("RGB double" + in_huge, "R'G'B' double" + in_huge, {0.0, -0.5e308, 1e308});
    CR_CHECK(encoded.size() == 3 && encoded[0] == 0.5 && std::fabs(encoded[1] - 0.25) < 1e-15 && encoded[2] == 1.0);
}

CR_TEST(values_stay_encoded_with_new_curves_only_within_one_model)
{
    // With a table, 1.5 decodes like 1. Within one model only the type changes
    // and values are kept; a model change decodes and encodes, so the value
    // becomes 1, as it does through the destination model in double.
    const auto table = *TransferFunction::sampled(std::vector<double>{0, 0.2, 1});
    const std::string in_table = "-" + spaceWith(table, "curve-encoded");
    const std::vector<double> values{1.5, -0.25, 0.5};
    const auto kept = convert<float>(("R'G'B' double" + in_table).c_str(), ("R'G'B' float" + in_table).c_str(), values);
    CR_CHECK(kept == (std::vector<float>{1.5f, -0.25f, 0.5f}));
    const auto changed = through("R'G'B' double" + in_table, "R'G'B'A double" + in_table, values);
    CR_CHECK(changed.size() == 4 && changed[0] == 1.0 && changed[1] == 0.0 && std::fabs(changed[2] - 0.5) < 1e-16 &&
             changed[3] == 1.0);
}
