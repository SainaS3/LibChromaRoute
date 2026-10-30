// Color spaces, transfer functions and formats with space (Milestone 3).

#include "support/test_harness.hpp"

#include "data/color_semantics_oracle_data.hpp"
#include "engine/transfer.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using chromaroute::ColorSpace;
using chromaroute::ErrorCode;
using chromaroute::PixelFormat;
using chromaroute::TransferFunction;

namespace oracle = chromaroute::test::oracle;

namespace {

ColorSpace space(std::string_view name)
{
    return *ColorSpace::fromName(name);
}

TransferFunction gamma(double g)
{
    return *TransferFunction::gamma(g);
}


} // namespace

CR_TEST(transfer_function_names)
{
    CR_CHECK(TransferFunction::srgb().name() == "sRGB");
    CR_CHECK(TransferFunction::linear().name() == "linear");
    CR_CHECK(gamma(2.2).name() == "2.2");
    CR_CHECK(gamma(1.8).name() == "1.8");
    CR_CHECK(gamma(2.4).name() == "2.399999"); // millionths truncated
    CR_CHECK(gamma(3.0).name() == "3.");
    CR_CHECK(gamma(0.45).name() == "0.45");
    CR_CHECK(gamma(1.005) == TransferFunction::linear()); // within 0.01 of 1
    CR_CHECK(gamma(2.2) == gamma(2.2));
    CR_CHECK(gamma(2.2000001) != gamma(2.2)); // interned by value, not by name
    CR_CHECK(gamma(2.2000001).name() == "2.2");
    CR_CHECK(*TransferFunction::fromName("2.2") == gamma(2.2)); // first of that name
    CR_CHECK(gamma(2.2).kind() == TransferFunction::Kind::gamma && gamma(2.2).gammaValue() == 2.2);
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        const auto t = TransferFunction::gamma(bad);
        CR_CHECK(!t.has_value() && t.error().code == ErrorCode::invalid_argument);
    }
    CR_CHECK(TransferFunction::fromName("nope").error().code == ErrorCode::unknown_transfer_function);
}

CR_TEST(gamma_names_are_defined_for_every_accepted_gamma)
{
    // Values beyond INT_MAX are named by the same digits rather than by an
    // overflowing int cast.
    CR_CHECK(gamma(2147483647.0).name() == "2147483647.");
    CR_CHECK(gamma(2147483647.5).name() == "2147483647.5");
    CR_CHECK(gamma(2147483648.0).name() == "2147483648.");
    CR_CHECK(gamma(3000000000.25).name() == "3000000000.25");
    CR_CHECK(gamma(1e20).name() == "100000000000000000000.");
    const TransferFunction largest = gamma(std::numeric_limits<double>::max());
    CR_CHECK(largest.name().size() == 310 && largest.name().substr(0, 17) == "17976931348623157" &&
             largest.name().back() == '.');
    CR_CHECK(largest.gammaValue() == std::numeric_limits<double>::max());
    CR_CHECK(gamma(std::numeric_limits<double>::denorm_min()).name() == "0.");
    // Extreme curves stay defined: x^(1/huge) tends to 1, x^(huge) to 0 on (0, 1).
    using namespace chromaroute::detail;
    CR_CHECK(fromLinear(largest.descriptor(), 0.5) == std::pow(0.5, 1.0 / std::numeric_limits<double>::max()));
    CR_CHECK(toLinear(largest.descriptor(), 0.5) == 0.0);
}

CR_TEST(gamma_curves_clamp_non_positive_values_and_keep_nan)
{
    using namespace chromaroute::detail;
    const TrcDescriptor& g = gamma(2.2).descriptor();
    CR_CHECK(toLinear(g, -0.5) == 0.0 && fromLinear(g, -0.5) == 0.0);
    CR_CHECK(toLinear(g, 0.0) == 0.0 && toLinear(g, 1.0) == 1.0);
    CR_CHECK(toLinear(g, 2.0) == std::pow(2.0, 2.2));
    CR_CHECK(std::isnan(toLinear(g, std::numeric_limits<double>::quiet_NaN())));
    CR_CHECK(std::isnan(fromLinear(g, std::numeric_limits<double>::quiet_NaN())));
}

// Normwise check: matrix elements are O(1), and rounding in the derivation
// (two 3x3 inversions, Bradford, four products) is absolute, so entries near 0
// are compared against the size of their row, in units of 2^-52.
CR_TEST(builtin_spaces_match_exact_derivation)
{
    double worst = 0;
    for (std::size_t s = 0; s < std::size(oracle::space_names); ++s) {
        const ColorSpace cs = space(oracle::space_names[s]);
        for (int r = 0; r < 3; ++r) {
            double row = 0;
            for (int c = 0; c < 3; ++c) {
                row = std::max(row, std::fabs(oracle::space_rgb_to_xyz[s][r * 3 + c]));
            }
            for (int c = 0; c < 3; ++c) {
                const double d = std::fabs(cs.rgbToXyz()[r][c] - oracle::space_rgb_to_xyz[s][r * 3 + c]);
                worst = std::max(worst, d / (row * 0x1p-52));
            }
        }
        const auto& y = cs.luminance();
        CR_CHECK(std::fabs(y[0] + y[1] + y[2] - 1.0) < 1e-14);
    }
    std::fprintf(cr_context.out(), "  built-in spaces: worst element %.1f x 2^-52 of its row from exact\n", worst);
    CR_CHECK(worst <= 16); // measured 2.6 (MSVC 19.51, 2026-10-24)
}

CR_TEST(builtin_spaces_have_expected_trcs)
{
    const struct {
        const char* name;
        const char* trc;
    } expected[] = {{"sRGB", "sRGB"},  {"scRGB", "linear"}, {"Rec2020", "sRGB"}, {"DisplayP3", "sRGB"},
                    {"Adobish", "2.2"}, {"ProPhoto", "1.8"},  {"Apple", "1.8"},   {"ACEScg", "linear"},
                    {"ACES2065-1", "linear"}};
    for (const auto& e : expected) {
        const ColorSpace cs = space(e.name);
        for (std::size_t ch = 0; ch < 3; ++ch) {
            CR_CHECK(cs.transfer(ch).name() == e.trc);
        }
        CR_CHECK(cs.name() == e.name);
    }
    CR_CHECK(ColorSpace::fromName("nope").error().code == ErrorCode::unknown_space);
    CR_CHECK(ColorSpace::srgb() == space("sRGB"));
}

CR_TEST(spaces_are_interned_by_parameters_and_named)
{
    const chromaroute::Chromaticities srgb{0.3127, 0.3290, 0.64, 0.33, 0.30, 0.60, 0.15, 0.06};
    const ColorSpace a = *ColorSpace::fromChromaticities("", srgb, gamma(2.4));
    CR_CHECK(a.name() == "space-0.3127,0.3290_0.6400,0.3300_0.1500,0.0600_0.3000,0.6000_2.399999,2.399999,2.399999");
    const ColorSpace b = *ColorSpace::fromChromaticities("IgnoredName", srgb, gamma(2.4));
    CR_CHECK(a == b && b.name() == a.name());
    CR_CHECK(!ColorSpace::fromName("IgnoredName").has_value());
    const ColorSpace mine = *ColorSpace::fromChromaticities("Mine", srgb, gamma(2.6));
    CR_CHECK(mine.name() == "Mine" && *ColorSpace::fromName("Mine") == mine);
    CR_CHECK(ColorSpace::srgb() == *ColorSpace::fromChromaticities("other", srgb, TransferFunction::srgb()));

    const ColorSpace rec_linear = space("Rec2020").withTransfer(TransferFunction::linear());
    CR_CHECK(rec_linear.name() == "space-0.3127,0.3290_0.7080,0.2920_0.1310,0.0460_0.1700,0.7970_linear,linear,linear");
    CR_CHECK(space("Rec2020").withTransfer(TransferFunction::srgb()) == space("Rec2020"));
    CR_CHECK(rec_linear.rgbToXyz() == space("Rec2020").rgbToXyz());

    const ColorSpace m = *ColorSpace::fromRgbToXyz(
        "", {0.9642, 1.0, 0.8249}, {{{0.4360, 0.3851, 0.1431}, {0.2225, 0.7169, 0.0606}, {0.0139, 0.0971, 0.7141}}},
        TransferFunction::srgb(), TransferFunction::srgb(), TransferFunction::srgb());
    CR_CHECK(m.name() == "space-0.9642,1.0000_0.4360,0.2225_0.1431,0.0606_0.3851,0.7169_sRGB,sRGB,sRGB");
    // Chromaticities derived from the matrix columns (0.345703 and 0.648424).
    CR_CHECK(std::fabs(m.chromaticities().white_x - 0.345703) < 1e-6 && std::fabs(m.chromaticities().red_x - 0.648424) < 1e-6);
    CR_CHECK(m.rgbToXyz()[1][1] == 0.7169); // the matrix is kept exactly

    // Parameters compare bit by bit: -0.0 is not +0.0.
    const chromaroute::Chromaticities ap0 = space("ACES2065-1").chromaticities();
    chromaroute::Chromaticities negative_zero = ap0;
    negative_zero.green_x = -0.0;
    CR_CHECK(ap0.green_x == 0.0 && !std::signbit(ap0.green_x));
    CR_CHECK(*ColorSpace::fromChromaticities("plus-zero", ap0, TransferFunction::linear()) == space("ACES2065-1"));
    const ColorSpace minus_zero = *ColorSpace::fromChromaticities("minus-zero", negative_zero, TransferFunction::linear());
    CR_CHECK(minus_zero != space("ACES2065-1") && minus_zero.name() == "minus-zero");

    // Names keep at most 511 characters.
    const std::string long_name(600, 'n');
    const ColorSpace long_named = *ColorSpace::fromChromaticities(long_name, {0.3127, 0.3290, 0.61, 0.33, 0.30, 0.60, 0.15, 0.06},
                                                                   TransferFunction::srgb());
    CR_CHECK(long_named.name() == long_name.substr(0, 511));
}

CR_TEST(invalid_spaces_are_rejected)
{
    const auto bad = [](const chromaroute::Chromaticities& c) {
        const auto s = ColorSpace::fromChromaticities("", c, TransferFunction::srgb());
        return !s.has_value() && s.error().code == ErrorCode::invalid_argument;
    };
    CR_CHECK(bad({0.3127, 0.0, 0.64, 0.33, 0.30, 0.60, 0.15, 0.06}));       // white y = 0
    CR_CHECK(bad({0.3127, 0.3290, 0.64, 0.33, 0.64, 0.33, 0.15, 0.06}));    // repeated primary
    CR_CHECK(bad({0.3127, 0.3290, std::numeric_limits<double>::quiet_NaN(), 0.33, 0.30, 0.60, 0.15, 0.06}));
    const auto singular = ColorSpace::fromRgbToXyz("", {0.9642, 1.0, 0.8249}, {{{1, 1, 1}, {1, 1, 1}, {0, 0, 1}}},
                                                      TransferFunction::srgb(), TransferFunction::srgb(),
                                                      TransferFunction::srgb());
    CR_CHECK(!singular.has_value());
    // A matrix column summing to 0 has no chromaticity: NaN, without dividing by zero.
    const auto zero_sum = ColorSpace::fromRgbToXyz("", {0.9642, 1.0, 0.8249}, {{{1, 0, 0}, {-1, 1, 0}, {0, 0, 1}}},
                                                      TransferFunction::srgb(), TransferFunction::srgb(),
                                                      TransferFunction::srgb());
    CR_CHECK(zero_sum.has_value() && std::isnan(zero_sum->chromaticities().red_x));
}

CR_TEST(formats_with_space_are_named_and_interned)
{
    const PixelFormat base = *PixelFormat::fromName("R'G'B'A u8");
    const PixelFormat rec = base.withSpace(space("Rec2020"));
    CR_CHECK(rec.name() == "R'G'B'A u8-Rec2020");
    CR_CHECK(rec.encodingName() == "R'G'B'A u8");
    CR_CHECK(rec.space() == space("Rec2020"));
    CR_CHECK(rec == base.withSpace(space("Rec2020")));
    CR_CHECK(rec.withSpace(ColorSpace::srgb()) == base);
    CR_CHECK(*PixelFormat::fromName("R'G'B'A u8-Rec2020") == rec);
    CR_CHECK(PixelFormat::fromName("RGBA float-ACES2065-1")->space() == space("ACES2065-1")); // '-' in the space name
    CR_CHECK(rec.withSpace(space("ProPhoto")).name() == "R'G'B'A u8-ProPhoto");
    CR_CHECK(rec.bytesPerPixel() == 4 && rec.encoding() == chromaroute::Encoding::space_trc);
}

CR_TEST(perceptual_models_always_use_the_srgb_curve)
{
    std::vector<std::uint8_t> v(256 * 3);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<std::uint8_t>(i / 3);
    }
    const auto decode = [&](const char* from, const char* to) {
        std::vector<float> out(v.size());
        const auto c = chromaroute::Converter::create(*PixelFormat::fromName(from), *PixelFormat::fromName(to));
        (void)c->process(std::as_bytes(std::span(v)), std::as_writable_bytes(std::span(out)), 256);
        return out;
    };
    const auto perceptual = decode("R~G~B~ u8-Adobish", "RGB float-Adobish");
    const auto nonlinear = decode("R'G'B' u8-Adobish", "RGB float-Adobish");
    const auto srgb = decode("R'G'B' u8", "RGB float");
    int mismatches = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        mismatches += std::bit_cast<std::uint32_t>(perceptual[i]) != std::bit_cast<std::uint32_t>(srgb[i]);
        mismatches += std::bit_cast<std::uint32_t>(nonlinear[i]) != oracle::gamma22_u8_to_linear_f32[i / 3];
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(channels_with_different_trcs_decode_and_encode_independently)
{
    // Linear red, sRGB green, gamma 2.2 blue: each channel must match a space
    // that uses its TRC on every channel, in both directions.
    using chromaroute::Converter;
    const ColorSpace srgb = ColorSpace::srgb();
    const TransferFunction gamma22 = *TransferFunction::gamma(2.2);
    const ColorSpace mixed = *ColorSpace::fromChromaticities("", srgb.chromaticities(), TransferFunction::linear(),
                                                               TransferFunction::srgb(), gamma22);
    std::vector<std::uint8_t> codes;
    for (int c = 0; c < 256; ++c) {
        codes.insert(codes.end(), 3, static_cast<std::uint8_t>(c));
    }
    const auto run = [](const char* from, const char* to, const ColorSpace& s, std::span<const std::byte> in,
                        std::span<std::byte> out) {
        const Converter c = *Converter::create(PixelFormat::fromName(from)->withSpace(s), PixelFormat::fromName(to)->withSpace(s));
        return static_cast<bool>(c.process(in, out, 256));
    };
    const auto decode = [&](const ColorSpace& s) {
        std::vector<double> out(codes.size());
        (void)run("R'G'B' u8", "RGB double", s, std::as_bytes(std::span(codes)), std::as_writable_bytes(std::span(out)));
        return out;
    };
    const auto decoded = decode(mixed);
    const auto linear = decode(srgb.withTransfer(TransferFunction::linear()));
    const auto curve = decode(srgb);
    const auto power = decode(srgb.withTransfer(gamma22));
    int differ = 0;
    for (std::size_t c = 0; c < 256; ++c) {
        differ += decoded[c * 3] != linear[c * 3] || decoded[c * 3 + 1] != curve[c * 3 + 1] ||
                  decoded[c * 3 + 2] != power[c * 3 + 2];
    }
    std::vector<std::uint8_t> encoded(codes.size());
    differ += !run("RGB double", "R'G'B' u8", mixed, std::as_bytes(std::span(decoded)), std::as_writable_bytes(std::span(encoded)));
    differ += encoded != codes;
    // A gray model in that space uses the first (red) TRC.
    std::vector<std::uint8_t> gray_codes(256);
    for (int c = 0; c < 256; ++c) {
        gray_codes[c] = static_cast<std::uint8_t>(c);
    }
    std::vector<double> gray(256);
    differ += !run("Y' u8", "Y double", mixed, std::as_bytes(std::span(gray_codes)), std::as_writable_bytes(std::span(gray)));
    for (std::size_t c = 0; c < 256; ++c) {
        differ += gray[c] != c / 255.0;
    }
    CR_CHECK(differ == 0);
}
