// RGB and gray ICC profiles built on lookup tables (Milestone 8c, decision
// A12): read through their tables by lcms2 in builds with
// CHROMAROUTE_WITH_LCMS2 when they have no matrix/TRC form, and unsupported
// without it.
//
// The profiles are written by the tests (support/lut_profiles.hpp). By
// default their device values are linear sRGB, so both tables are linear maps
// and the expected values are the device values themselves; lcms2's
// tetrahedral interpolation reproduces a linear map, which leaves its 16-bit
// rounding (cmslut.c, EvaluateCLUTfloatIn16): of the inputs, of the stored
// nodes and of the results. The grids (18 and 52 points) put every node on a
// whole 16-bit step.

#include "support/conversion_checks.hpp"
#include "support/lut_profiles.hpp"
#include "support/test_harness.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

using chromaroute::ColorSpace;
using chromaroute::Converter;
using chromaroute::ErrorCode;
using chromaroute::PixelFormat;
using chromaroute::Vector3;
using chromaroute::test::IccWriter;
using namespace chromaroute::test;

namespace {

constexpr double table_step = 1.0 / 65535.0;

const ColorSpace& linearSpace()
{
    static const ColorSpace space = *ColorSpace::fromIccProfile("lutRGB", lutRgbProfile().bytes());
    return space;
}

// The largest row sum of |M^-1|: how much an XYZ error grows in linear RGB.
double inverseNorm()
{
    double norm = 0;
    for (const auto& row : scrgb_inverse) {
        norm = std::max(norm, std::fabs(row[0]) + std::fabs(row[1]) + std::fabs(row[2]));
    }
    return norm;
}

// Decoding the linear profile, per RGB component: half an XYZ step (1/32768)
// for the stored nodes and half for lcms2's 16-bit result, through M^-1; and
// half a step of each device input, which a linear map passes on unchanged.
double decodeBound()
{
    return inverseNorm() * (1.0 / 32768.0) + 0.5 * table_step + 1e-6;
}

// Encoding into it: half an XYZ step for the rounded input, through M^-1;
// half a step for the stored node and half for the result.
double encodeBound()
{
    return inverseNorm() * (0.5 / 32768.0) + table_step + 1e-6;
}

std::vector<double> randomUnit(std::size_t count, unsigned seed, double lo = 0.0, double hi = 1.0)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> unit(lo, hi);
    std::vector<double> v(count);
    for (double& x : v) {
        x = unit(random);
    }
    return v;
}

double worstDifference(const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size() || a.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = worstOf(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

} // namespace

CR_TEST(lut_profiles_need_lcms2)
{
    const auto rgb = ColorSpace::fromIccProfile("lut-rgb-check", lutRgbProfile({2, 2}).bytes());
    const auto gray = ColorSpace::fromIccProfile("lut-gray-check", lutGrayProfile(2, 2).bytes());
#if CHROMAROUTE_TEST_WITH_LCMS2
    CR_CHECK(rgb.has_value() && rgb->hasLookupTableProfile() && !rgb->hasCmykProfile());
    CR_CHECK(gray.has_value() && gray->hasLookupTableProfile() && !gray->hasCmykProfile());
#else
    CR_CHECK(!rgb && rgb.error().code == ErrorCode::unsupported_profile &&
             rgb.error().message.find("lcms2") != std::string::npos);
    CR_CHECK(!gray && gray.error().code == ErrorCode::unsupported_profile);
    CR_CHECK(!ColorSpace::fromName("lut-rgb-check").has_value() && !ColorSpace::fromName("lut-gray-check").has_value());
#endif
    CR_CHECK(!ColorSpace::srgb().hasLookupTableProfile() && !ColorSpace::fromName("scRGB")->hasLookupTableProfile());
}

CR_TEST(profiles_with_a_matrix_trc_form_keep_it)
{
    // A display profile with both a matrix/TRC form and (swapped) lookup
    // tables is read as matrix/TRC in every build (A12; babl does so in its
    // speed mode only).
    LutRgbOptions swapped;
    swapped.swap = true;
    IccWriter both = IccWriter::rgb([](IccWriter& w, const char* s) { w.para(s, 0, {s15(1.0)}); }, 2);
    both.lut16("A2B0", 3, 3, 2, [&](const std::vector<double>& in) {
        return lutPcsValues(swapped, chromaroute::detail::multiply(scrgb_matrix, lutLinear(swapped, in[0], in[1], in[2])));
    });
    const auto space = ColorSpace::fromIccProfile("", both.bytes());
    CR_CHECK(space.has_value() && !space->hasLookupTableProfile());
    if (space) {
        const std::vector<double> rgb{0.25, 0.5, 0.75};
        const std::string format = "R'G'B' double-" + std::string(space->name());
        const auto lab = convert<double>(format.c_str(), "CIE Lab double", rgb);
        const auto staged = convert<double>(("RGB double-" + std::string(space->name())).c_str(), "CIE Lab double", rgb);
        CR_CHECK(lab == staged); // its linear TRC, not the swapped table
    }
}

#if CHROMAROUTE_TEST_WITH_LCMS2

CR_TEST(lut_profile_spaces_are_srgb_with_their_profile)
{
    const ColorSpace& space = linearSpace();
    const ColorSpace srgb = ColorSpace::srgb();
    CR_CHECK(space.name() == "lutRGB" && space != srgb && space.rgbToXyz() == srgb.rgbToXyz());
    for (std::size_t ch = 0; ch < 3; ++ch) {
        CR_CHECK(space.transfer(ch) == srgb.transfer(ch));
    }
    // Interned by the profile's bytes; unnamed ones are named by their kind.
    const auto again = ColorSpace::fromIccProfile("another-lut-name", lutRgbProfile().bytes());
    CR_CHECK(again.has_value() && *again == space && again->name() == "lutRGB");
    const auto unnamed_rgb = ColorSpace::fromIccProfile("", lutRgbProfile({2, 3}).bytes());
    const auto unnamed_gray = ColorSpace::fromIccProfile("", lutGrayProfile(2, 3).bytes());
    CR_CHECK(unnamed_rgb.has_value() && unnamed_rgb->name().substr(0, 4) == "rgb-");
    CR_CHECK(unnamed_gray.has_value() && unnamed_gray->name().substr(0, 5) == "gray-");
    // Another TRC gives a space without the profile; sRGB's keeps it.
    CR_CHECK(!space.withTransfer(chromaroute::TransferFunction::linear()).hasLookupTableProfile());
    CR_CHECK(space.withTransfer(chromaroute::TransferFunction::srgb()) == space);

    // Every model but the encoded RGB ones is sRGB's in it: linear RGB, the
    // perceptual and gray models, CMYK (naive), CIE.
    std::vector<std::uint8_t> bytes(4 * 300);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }
    for (const char* model : {"RGBA u8", "R~G~B~A u8", "Y'A u8", "YA u8", "cmyk u8"}) {
        const std::string in_space = std::string(model) + "-lutRGB";
        CR_CHECK(convert<double>(in_space.c_str(), "CIE Lab alpha double", bytes) ==
                 convert<double>(model, "CIE Lab alpha double", bytes));
    }
    // In a gray profile's space the RGB models are sRGB's.
    const ColorSpace& gray = *unnamed_gray;
    const std::string gray_rgb = "R'G'B'A u8-" + std::string(gray.name());
    CR_CHECK(convert<std::uint8_t>(gray_rgb.c_str(), "R'G'B'A u8", bytes) == bytes);
}

CR_TEST(lookup_tables_are_read_where_matrix_trc_is_not)
{
    // Each of these has no matrix/TRC form to read, and its tables open.
    struct Case {
        const char* what;
        std::vector<std::byte> bytes;
    };
    // A display profile missing one matrix/TRC tag, with tables.
    LutRgbOptions small{2, 2};
    small.device_class = "mntr";
    IccWriter partial = lutRgbProfile(small);
    partial.xyz("rXYZ", 0.4360747, 0.2225045, 0.0139322).xyz("gXYZ", 0.3850649, 0.7168786, 0.0971045);
    const std::vector<Case> cases = {
        {"output RGB", lutRgbProfile({2, 2}).bytes()},
        {"display RGB with tables only", lutRgbProfile({2, 2, false, false, 0, false, "mntr"}).bytes()},
        {"input RGB with tables only", lutRgbProfile({2, 2, false, false, 0, false, "scnr"}).bytes()},
        {"color space RGB", lutRgbProfile({2, 2, false, false, 0, false, "spac"}).bytes()},
        {"Lab connection space", lutRgbProfile({2, 2, false, false, 0, true, "mntr"}).bytes()},
        {"display RGB missing a matrix/TRC tag", partial.bytes()},
        {"color space gray", lutGrayProfile(2, 2).bytes()},
        {"output gray with tables only", lutGrayProfile(2, 2, false, "prtr").bytes()},
    };
    for (const Case& c : cases) {
        const auto space = ColorSpace::fromIccProfile("", c.bytes);
        const bool ok = space.has_value() && space->hasLookupTableProfile() && !space->hasCmykProfile();
        if (!ok) {
            std::fprintf(cr_context.out(), "  %s: %s\n", c.what, space ? "not a lookup-table space" : space.error().message.c_str());
        }
        CR_CHECK(ok);
    }
}

CR_TEST(lut_rgb_profile_decodes_through_its_tables)
{
    (void)linearSpace();
    // The nodes, then random device values.
    std::vector<double> device;
    for (int r = 0; r <= 17; ++r) {
        for (int g = 0; g <= 17; g += 3) {
            for (int b = 0; b <= 17; b += 4) {
                device.insert(device.end(), {r / 17.0, g / 17.0, b / 17.0});
            }
        }
    }
    const auto random = randomUnit(3 * 3000, 12);
    device.insert(device.end(), random.begin(), random.end());
    const auto rgb = convert<double>("R'G'B' double-lutRGB", "RGB double-scRGB", device);
    const double worst = worstDifference(rgb, device);
    std::fprintf(cr_context.out(), "  LUT RGB decode: worst %.3g (bound %.3g)\n", worst, decodeBound());
    CR_CHECK(worst <= decodeBound());
    // The alpha forms meet the same tables: straight alpha is kept, and
    // associated values (alpha 1/2, exact) are un-associated first.
    std::vector<double> straight;
    std::vector<double> associated;
    for (std::size_t p = 0; p < 200; ++p) {
        straight.insert(straight.end(), {device[3 * p], device[3 * p + 1], device[3 * p + 2], 0.5});
        associated.insert(associated.end(), {device[3 * p] / 2, device[3 * p + 1] / 2, device[3 * p + 2] / 2, 0.5});
    }
    const auto from_straight = convert<double>("R'G'B'A double-lutRGB", "RGBA double-scRGB", straight);
    CR_CHECK(from_straight == convert<double>("R'aG'aB'aA double-lutRGB", "RGBA double-scRGB", associated));
    // Associated on both sides: un-associated before the profile, associated
    // again after it, never kept across it.
    const auto both = convert<double>("R'aG'aB'aA double-lutRGB", "RaGaBaA double-scRGB", associated);
    bool reassociated = both.size() == from_straight.size();
    for (std::size_t i = 0; reassociated && i < both.size(); ++i) {
        reassociated = both[i] == (i % 4 == 3 ? 0.5 : from_straight[i] * 0.5);
    }
    CR_CHECK(reassociated);
    bool same = from_straight.size() == 800;
    for (std::size_t p = 0; same && p < 200; ++p) {
        same = from_straight[4 * p] == rgb[3 * p] && from_straight[4 * p + 1] == rgb[3 * p + 1] &&
               from_straight[4 * p + 2] == rgb[3 * p + 2] && from_straight[4 * p + 3] == 0.5;
    }
    CR_CHECK(same);

    // A curved profile (linear = device^2): between nodes the interpolation
    // misses the curve by up to h^2 / 4 per component, h = 1/17; the input's
    // half step is doubled by the slope.
    LutRgbOptions curved;
    curved.squared = true;
    const auto squared = ColorSpace::fromIccProfile("lutRGBsquared", lutRgbProfile(curved).bytes());
    CR_CHECK(squared.has_value());
    const auto curved_rgb = convert<double>("R'G'B' double-lutRGBsquared", "RGB double-scRGB", random);
    std::vector<double> expected(random.size());
    std::transform(random.begin(), random.end(), expected.begin(), [](double x) { return x * x; });
    const double h = 1.0 / 17;
    const double curved_bound = decodeBound() + h * h / 4 + 0.5 * table_step;
    const double worst_curved = worstDifference(curved_rgb, expected);
    std::fprintf(cr_context.out(), "  LUT RGB curved decode: worst %.3g (bound %.3g)\n", worst_curved, curved_bound);
    CR_CHECK(worst_curved <= curved_bound);
}

CR_TEST(lut_rgb_profile_encodes_through_its_tables)
{
    (void)linearSpace();
    // Linear colors well inside the cube, so every B2A0 cell they fall in has
    // its nodes inside it too, and the table is the exact inverse map there.
    const auto rgb = randomUnit(3 * 3000, 13, 0.25, 0.75);
    const auto device = convert<double>("RGB double-scRGB", "R'G'B' double-lutRGB", rgb);
    const double worst = worstDifference(device, rgb);
    std::fprintf(cr_context.out(), "  LUT RGB encode: worst %.3g (bound %.3g)\n", worst, encodeBound());
    CR_CHECK(worst <= encodeBound());
    // Associated alpha after the profile; alpha is kept.
    std::vector<double> with_alpha;
    for (std::size_t p = 0; p < 100; ++p) {
        with_alpha.insert(with_alpha.end(), {rgb[3 * p], rgb[3 * p + 1], rgb[3 * p + 2], 0.25});
    }
    const auto straight = convert<double>("RGBA double-scRGB", "R'G'B'A double-lutRGB", with_alpha);
    const auto associated = convert<double>("RGBA double-scRGB", "R'aG'aB'aA double-lutRGB", with_alpha);
    bool associates = straight.size() == 400 && associated.size() == 400;
    for (std::size_t i = 0; associates && i < 400; ++i) {
        associates = i % 4 == 3 ? associated[i] == 0.25 && straight[i] == 0.25 : associated[i] == straight[i] * 0.25;
    }
    CR_CHECK(associates);
    // Round trips from device values: the two bounds, the second through the
    // map's unit slope.
    const auto back = convert<double>("RGB double-scRGB", "R'G'B' double-lutRGB",
                                      convert<double>("R'G'B' double-lutRGB", "RGB double-scRGB", rgb));
    CR_CHECK(worstDifference(back, rgb) <= decodeBound() + encodeBound());
}

CR_TEST(lut_rgb_profiles_with_a_lab_connection_space)
{
    // At A2B0's nodes the stored Lab is off by half an encoding step (L
    // 100/65280, a and b 1/256) and lcms2's 16-bit result by another half.
    // Through Lab to XYZ, dX = Xn x 3 f^2 (dL / 116 + da / 500) with f <= 1,
    // likewise Y (dL only) and Z (db / 200); then M^-1, plus 1e-5 for lcms2's
    // D50 beside the engine's exact one. Device values at nodes are exact
    // 16-bit inputs.
    const double dl = 100.0 / 65280.0;
    const double dab = 1.0 / 256.0;
    const double dxyz = std::max({lcms_d50[0] * 3 * (dl / 116 + dab / 500), 3 * dl / 116,
                                  lcms_d50[2] * 3 * (dl / 116 + dab / 200)});
    const double bound = inverseNorm() * dxyz + 1e-5;
    LutRgbOptions o;
    o.lab = true;
    o.b2a = 18;
    const auto lab = ColorSpace::fromIccProfile("labLUT", lutRgbProfile(o).bytes());
    CR_CHECK(lab.has_value() && lab->hasLookupTableProfile());
    std::vector<double> device;
    for (int r = 0; r <= 17; ++r) {
        for (int g = 0; g <= 17; ++g) {
            for (int b = 0; b <= 17; b += 2) {
                device.insert(device.end(), {r / 17.0, g / 17.0, b / 17.0});
            }
        }
    }
    const double worst = worstDifference(convert<double>("R'G'B' double-labLUT", "RGB double-scRGB", device), device);
    std::fprintf(cr_context.out(), "  LUT RGB with Lab: worst %.3g at nodes (bound %.3g)\n", worst, bound);
    CR_CHECK(worst <= bound);
}

CR_TEST(lut_rgb_profile_black_point_is_compensated)
{
    // A profile whose black is 5% of white. Black point compensation maps it
    // to scRGB's black, 0, and scales the rest with it (A12, as A11), so the
    // profile decodes to the device values again, within the decoding bound
    // scaled by 1 / 0.95 as the tables' steps are. Without it every color
    // would keep the lift, up to 0.05.
    //
    // lcms2 finds a black point only through a table for the intent, or a
    // matrix (cmssamp.c, BlackPointAsDarkerColorant, which asks
    // cmsIsIntentSupported): an RGB profile that reaches relative
    // colorimetric through A2B0 alone gets a black of 0, so compensation does
    // nothing and the lift stays. Both are checked.
    constexpr double black = 0.05;
    LutRgbOptions o;
    o.black = black;
    o.relative = true;
    const auto lifted = ColorSpace::fromIccProfile("liftedLUT", lutRgbProfile(o).bytes());
    o.relative = false;
    const auto lifted_a2b0 = ColorSpace::fromIccProfile("liftedLUTA2B0", lutRgbProfile(o).bytes());
    CR_CHECK(lifted.has_value() && lifted_a2b0.has_value());
    std::vector<double> device{0, 0, 0, 1, 1, 1};
    const auto random = randomUnit(3 * 1000, 14);
    device.insert(device.end(), random.begin(), random.end());
    const auto rgb = convert<double>("R'G'B' double-liftedLUT", "RGB double-scRGB", device);
    const auto rgb_a2b0 = convert<double>("R'G'B' double-liftedLUTA2B0", "RGB double-scRGB", device);
    const double bound = decodeBound() / (1 - black) + 1e-6;
    const double worst = worstDifference(rgb, device);
    std::fprintf(cr_context.out(), "  LUT RGB black point compensation: worst %.3g (bound %.3g); A2B0 only: black %.3g\n",
                 worst, bound, rgb_a2b0.empty() ? -1.0 : rgb_a2b0[0]);
    CR_CHECK(worst <= bound);
    std::vector<double> kept(device.size());
    std::transform(device.begin(), device.end(), kept.begin(), [](double x) { return black + (1 - black) * x; });
    CR_CHECK(worstDifference(rgb_a2b0, kept) <= decodeBound());
}

CR_TEST(lut_gray_profile_converts_through_its_tables)
{
    // Gray y is y x the D50 white, which is scRGB (y, y, y); 1e-5 allows for
    // lcms2's D50 beside the engine's exact one.
    const auto gray = ColorSpace::fromIccProfile("lutGray", lutGrayProfile().bytes());
    CR_CHECK(gray.has_value() && gray->hasLookupTableProfile());
    std::vector<double> y;
    for (int i = 0; i <= 17; ++i) {
        y.push_back(i / 17.0);
    }
    const auto random = randomUnit(1000, 15);
    y.insert(y.end(), random.begin(), random.end());
    const auto rgb = convert<double>("Y' double-lutGray", "RGB double-scRGB", y);
    std::vector<double> expected;
    for (double v : y) {
        expected.insert(expected.end(), {v, v, v});
    }
    const double bound = decodeBound() + 1e-5;
    const double worst = worstDifference(rgb, expected);
    std::fprintf(cr_context.out(), "  LUT gray decode: worst %.3g (bound %.3g)\n", worst, bound);
    CR_CHECK(worst <= bound);
    // Back from neutral scRGB: Y is y, within the encoding bound. B2A0's cell
    // around Y = 1 (Y from 25/51 to 26/51 of the table's range, 0.980 to
    // 1.020) has its upper nodes clamped to 1, which bends the table there, so
    // the values stay below 0.98.
    const auto inner = randomUnit(1000, 18, 0.0, 0.98);
    const auto neutral = convert<double>("Y' double-lutGray", "RGB double-scRGB", inner);
    const auto back = convert<double>("RGB double-scRGB", "Y' double-lutGray", neutral);
    const double round_trip = worstDifference(back, inner);
    std::fprintf(cr_context.out(), "  LUT gray round trip: worst %.3g (bound %.3g)\n", round_trip, bound + encodeBound());
    CR_CHECK(round_trip <= bound + encodeBound());
    // Gray formats in it to linear Y in sRGB: the luminance of (y, y, y).
    const auto luminance = convert<double>("Y' double-lutGray", "Y double", random);
    CR_CHECK(worstDifference(luminance, random) <= bound);
    // A gray profile need not be neutral: one whose gray is a tinted XYZ gives
    // that color, M^-1 (y x tint), not three equal values.
    const Vector3 tint{0.9, 1.0, 0.6};
    const auto tinted = ColorSpace::fromIccProfile("lutGrayTinted", lutGrayProfile(18, 18, false, "spac", tint).bytes());
    CR_CHECK(tinted.has_value());
    const auto tinted_rgb = convert<double>("Y' double-lutGrayTinted", "RGB double-scRGB", random);
    std::vector<double> tinted_expected;
    for (double v : random) {
        const Vector3 xyz{v * tint[0], v * tint[1], v * tint[2]};
        const Vector3 c = chromaroute::detail::multiply(scrgb_inverse, xyz);
        tinted_expected.insert(tinted_expected.end(), {c[0], c[1], c[2]});
    }
    const double tinted_worst = worstDifference(tinted_rgb, tinted_expected);
    std::fprintf(cr_context.out(), "  LUT tinted gray decode: worst %.3g (bound %.3g)\n", tinted_worst, bound);
    CR_CHECK(tinted_worst <= bound);
}

CR_TEST(lut_profiles_input_only)
{
    // No B2A0 (A12): the space opens and decodes; a conversion that must
    // encode into it fails at Converter::create with unsupported_profile.
    const auto input_only = ColorSpace::fromIccProfile("inputOnlyLUT", lutRgbProfile({}, false).bytes());
    CR_CHECK(input_only.has_value() && input_only->hasLookupTableProfile());
    const auto random = randomUnit(3 * 500, 16);
    CR_CHECK(worstDifference(convert<double>("R'G'B' double-inputOnlyLUT", "RGB double-scRGB", random), random) <=
             decodeBound());
    const auto into = Converter::create(*PixelFormat::fromName("R'G'B' u8"), *PixelFormat::fromName("R'G'B' u8-inputOnlyLUT"));
    CR_CHECK(!into && into.error().code == ErrorCode::unsupported_profile &&
             into.error().message.find("inputOnlyLUT") != std::string::npos);
    const auto from_other = Converter::create(*PixelFormat::fromName("R'G'B'A float-lutRGB"),
                                              *PixelFormat::fromName("R'aG'aB'aA float-inputOnlyLUT"));
    CR_CHECK(!from_other && from_other.error().code == ErrorCode::unsupported_profile);
    // Formats of the profile itself keep their device values, and every
    // model that is sRGB's in its space converts into it.
    std::vector<std::uint8_t> u8{0, 64, 128, 255, 1, 2};
    const auto floats = convert<float>("R'G'B' u8-inputOnlyLUT", "R'G'B' float-inputOnlyLUT", u8);
    bool kept = floats.size() == 6;
    for (std::size_t i = 0; kept && i < 6; ++i) {
        kept = floats[i] == static_cast<float>(u8[i] / 255.0);
    }
    CR_CHECK(kept);
    CR_CHECK(Converter::create(*PixelFormat::fromName("R'G'B' u8"), *PixelFormat::fromName("RGB float-inputOnlyLUT")));
}

CR_TEST(lut_profile_device_values_are_kept_within_a_profile)
{
    (void)linearSpace();
    // Between formats of one profile only the type and association change,
    // without the tables (they would only add rounding).
    std::vector<std::uint8_t> u8(3 * 256);
    for (std::size_t i = 0; i < u8.size(); ++i) {
        u8[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    CR_CHECK(convert<std::uint8_t>("R'G'B' u16-lutRGB", "R'G'B' u8-lutRGB",
                                   convert<std::uint16_t>("R'G'B' u8-lutRGB", "R'G'B' u16-lutRGB", u8)) == u8);
    CR_CHECK(convert<double>("R'G'B' u8-lutRGB", "R'G'B' double-lutRGB", u8) ==
             convert<double>("R'G'B' u8", "R'G'B' double", u8));
    const std::vector<double> associated{0.125, 0.25, 0.375, 0.5, 0.0, 0.0, 0.0, 0.0};
    const auto straight = convert<double>("R'aG'aB'aA double-lutRGB", "R'G'B'A double-lutRGB", associated);
    CR_CHECK(straight.size() == 8 && straight[0] == 0.25 && straight[1] == 0.5 && straight[2] == 0.75 &&
             straight[3] == 0.5);
}

CR_TEST(lut_rgb_between_profiles_goes_through_both)
{
    (void)linearSpace();
    // The swapped profile exchanges red and green; between the two the
    // colors meet in linear scRGB (A12), so red and green trade places,
    // within a decoding and an encoding bound.
    LutRgbOptions o;
    o.swap = true;
    const auto swapped = ColorSpace::fromIccProfile("swappedLUT", lutRgbProfile(o).bytes());
    CR_CHECK(swapped.has_value());
    const auto device = randomUnit(3 * 2000, 17, 0.25, 0.75);
    const auto out = convert<double>("R'G'B' double-lutRGB", "R'G'B' double-swappedLUT", device);
    std::vector<double> expected(device.size());
    for (std::size_t p = 0; p < device.size() / 3; ++p) {
        expected[3 * p] = device[3 * p + 1];
        expected[3 * p + 1] = device[3 * p];
        expected[3 * p + 2] = device[3 * p + 2];
    }
    const double worst = worstDifference(out, expected);
    std::fprintf(cr_context.out(), "  LUT RGB between profiles: worst %.3g (bound %.3g)\n", worst,
                 decodeBound() + encodeBound());
    CR_CHECK(worst <= decodeBound() + encodeBound());
    // Equal to the staged route through scRGB.
    CR_CHECK(out == convert<double>("RGB double-scRGB", "R'G'B' double-swappedLUT",
                                    convert<double>("R'G'B' double-lutRGB", "RGB double-scRGB", device)));
    // Gray profile to RGB profile: a neutral through both.
    const auto gray = ColorSpace::fromIccProfile("lutGrayToRgb", lutGrayProfile(18, 52, false, "prtr").bytes());
    CR_CHECK(gray.has_value());
    const std::vector<double> y{0.25, 0.5, 0.75};
    const auto rgb = convert<double>("Y' double-lutGrayToRgb", "R'G'B' double-lutRGB", y);
    CR_CHECK(worstDifference(rgb, {0.25, 0.25, 0.25, 0.5, 0.5, 0.5, 0.75, 0.75, 0.75}) <=
             decodeBound() + encodeBound() + 1e-5);
}

CR_TEST(lut_profile_non_finite_values)
{
    (void)linearSpace();
    // A NaN or infinite component gives NaN in every color, both ways; alpha is kept.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const auto rgb = convert<double>("R'G'B'A double-lutRGB", "RGBA double-scRGB",
                                     std::vector<double>{nan, 0, 0, 1, 0, inf, 0, 0.5, 0.5, 0.5, -inf, 1});
    bool all_nan = rgb.size() == 12;
    for (std::size_t p = 0; all_nan && p < 3; ++p) {
        all_nan = std::isnan(rgb[4 * p]) && std::isnan(rgb[4 * p + 1]) && std::isnan(rgb[4 * p + 2]);
    }
    CR_CHECK(all_nan && rgb[3] == 1 && rgb[7] == 0.5);
    const auto device = convert<double>("RGBA double-scRGB", "R'G'B'A double-lutRGB",
                                        std::vector<double>{nan, 0.5, 0.5, 1, 0.5, -inf, 0.5, 0.25});
    all_nan = device.size() == 8;
    for (std::size_t p = 0; all_nan && p < 2; ++p) {
        all_nan = std::isnan(device[4 * p]) && std::isnan(device[4 * p + 1]) && std::isnan(device[4 * p + 2]);
    }
    CR_CHECK(all_nan && device[3] == 1 && device[7] == 0.25);
}

CR_TEST(lut_profiles_with_problems_are_rejected)
{
    struct Case {
        const char* what;
        std::vector<std::byte> bytes;
        ErrorCode code;
    };
    IccWriter only_b2a(2, "RGB ", "XYZ ", "prtr");
    only_b2a.xyz("wtpt", 0.9642, 1.0, 0.8249);
    only_b2a.lut16("B2A0", 3, 3, 2, [](const std::vector<double>& in) { return in; });
    IccWriter wrong_type(2, "RGB ", "XYZ ", "prtr");
    wrong_type.xyz("wtpt", 0.9642, 1.0, 0.8249).raw("A2B0", std::vector<std::byte>(64));
    IccWriter only_saturation(2, "RGB ", "XYZ ", "prtr");
    only_saturation.xyz("wtpt", 0.9642, 1.0, 0.8249);
    only_saturation.lut16("A2B2", 3, 3, 2, [](const std::vector<double>& in) { return in; });
    const std::vector<Case> cases = {
        {"no table into the connection space", only_b2a.bytes(), ErrorCode::unsupported_profile},
        {"only a saturation table", only_saturation.bytes(), ErrorCode::unsupported_profile},
        {"A2B0 of no known type", wrong_type.bytes(), ErrorCode::invalid_profile},
        {"output RGB without tables", IccWriter(2, "RGB ", "XYZ ", "prtr").xyz("wtpt", 0.9642, 1.0, 0.8249).bytes(),
         ErrorCode::unsupported_profile},
        {"color space gray without tables", IccWriter(2, "GRAY", "XYZ ", "spac").bytes(), ErrorCode::unsupported_profile},
        {"Lab connection space without tables", IccWriter(2, "RGB ", "Lab ", "mntr").bytes(),
         ErrorCode::unsupported_profile},
    };
    int index = 0;
    for (const Case& c : cases) {
        const std::string name = "rejected-lut-" + std::to_string(index++);
        const auto space = ColorSpace::fromIccProfile(name, c.bytes);
        const bool ok = !space && space.error().code == c.code && !space.error().message.empty() &&
                        !ColorSpace::fromName(name).has_value();
        if (!ok) {
            std::fprintf(cr_context.out(), "  %s: %s\n", c.what, space ? "accepted" : space.error().message.c_str());
        }
        CR_CHECK(ok);
    }
}

CR_TEST(lut_profile_corruptions_never_misbehave)
{
    // Every truncation of small RGB and gray profiles, with the declared size
    // made to match, and 2,000 random corruptions of 1 to 8 bytes of the RGB
    // one: each gives a space or a profile error, and a space converts both
    // ways (or reports an input-only profile) without misbehaving.
    int bad = 0;
    for (const auto& valid : {lutRgbProfile({2, 3}).bytes(), lutGrayProfile(2, 3).bytes()}) {
        for (std::size_t size = 0; size < valid.size(); ++size) {
            auto cut = std::vector<std::byte>(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(size));
            if (size >= 4) {
                IccWriter::set32(cut, 0, static_cast<std::uint32_t>(size));
            }
            const auto space = ColorSpace::fromIccProfile("", cut);
            bad += space.has_value() && !space->hasLookupTableProfile();
            bad += !space && space.error().code != ErrorCode::invalid_profile &&
                   space.error().code != ErrorCode::unsupported_profile;
        }
    }
    const auto valid = lutRgbProfile({2, 3}).bytes();
    std::mt19937 random(2027);
    int spaces = 0;
    int errors = 0;
    for (int i = 0; i < 2000; ++i) {
        auto bytes = valid;
        const int changes = 1 + static_cast<int>(random() % 8);
        for (int c = 0; c < changes; ++c) {
            bytes[random() % bytes.size()] = static_cast<std::byte>(random() & 0xFF);
        }
        const auto space = ColorSpace::fromIccProfile("", bytes);
        if (space) {
            ++spaces;
            if (!space->hasLookupTableProfile()) {
                continue; // a corruption may complete another form, which M8a tests
            }
            const std::string format = "R'G'B' u8-" + std::string(space->name());
            bad += convert<std::uint8_t>(format.c_str(), "R'G'B' u8", std::vector<std::uint8_t>{0, 64, 128}).size() != 3;
            const auto into = Converter::create(*PixelFormat::fromName("R'G'B' u8"), *PixelFormat::fromName(format));
            if (into) {
                std::vector<std::uint8_t> out(3);
                const std::vector<std::uint8_t> in{0, 64, 128};
                bad += !into->process(std::as_bytes(std::span(in)), std::as_writable_bytes(std::span(out)), 1);
            } else {
                bad += into.error().code != ErrorCode::unsupported_profile;
            }
        } else if (space.error().code == ErrorCode::invalid_profile ||
                   space.error().code == ErrorCode::unsupported_profile) {
            ++errors;
        } else {
            ++bad;
        }
    }
    std::fprintf(cr_context.out(), "  corrupted LUT RGB profiles: %d accepted, %d rejected\n", spaces, errors);
    CR_CHECK(bad == 0);
}

#endif
