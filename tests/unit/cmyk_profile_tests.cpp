// CMYK ICC profiles (Milestone 8b, decision A11): spaces whose CMYK formats
// convert through the profile, evaluated by lcms2 in builds with
// CHROMAROUTE_WITH_LCMS2, and unsupported without it.
//
// The profiles are written by the tests (support/naive_cmyk_profile.hpp).
// Their tables hold the naive formula (S5) with XYZ as the connection space,
// so expected values are known: the formula itself between nodes, within the
// interpolation's error, and the stored values at the nodes. lcms2 evaluates
// 16-bit tables with 16-bit inputs and outputs (cmslut.c,
// EvaluateCLUTfloatIn16), even in a double transform, so the bounds below
// count in steps of 1/65535 of a table.

#include "support/conversion_checks.hpp"
#include "support/naive_cmyk_profile.hpp"
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
#include <thread>
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
constexpr std::size_t a2b_grid = 17;
constexpr std::size_t b2a_grid = 33;

std::vector<std::byte> naiveProfile(std::size_t a2b = a2b_grid, std::size_t b2a = b2a_grid, bool swap = false)
{
    return naiveCmykProfile(a2b, b2a, swap).bytes();
}

// The spaces of the default profile and of its swapped twin, created once.
const ColorSpace& naiveSpace()
{
    static const ColorSpace space = *ColorSpace::fromIccProfile("naiveCMYK", naiveProfile());
    return space;
}

const ColorSpace& swappedSpace()
{
    static const ColorSpace space = *ColorSpace::fromIccProfile("swappedCMYK", naiveProfile(a2b_grid, b2a_grid, true));
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

// Decoding at A2B0's nodes, per RGB component: half a step of XYZ for the
// stored node, half for lcms2's 16-bit result, and half a step of each of two
// inks after lcms2 rounds them to 16 bits (|dXYZ / dink| <= 1), through M^-1.
double decodeBoundAtNodes()
{
    return inverseNorm() * 3 * (0.5 / 32768.0) + 1e-6;
}

// Between nodes, add the interpolation's error: lcms2 interpolates the last
// three inks by tetrahedra, which misses the bilinear terms (1 - m)(1 - k)
// and (1 - y)(1 - k) by at most h^2 / 4 per RGB component.
double decodeBound()
{
    const double h = 1.0 / (a2b_grid - 1);
    return decodeBoundAtNodes() + h * h / 4;
}

// The linear scRGB colors in the unit cube whose XYZ is a B2A0 node, every
// `step`-th node along X.
std::vector<double> nodeColors(std::size_t step)
{
    std::vector<double> rgb;
    const double d = static_cast<double>(b2a_grid - 1);
    for (std::size_t i = 0; i < b2a_grid; i += step) {
        for (std::size_t j = 0; j < b2a_grid; ++j) {
            for (std::size_t l = 0; l < b2a_grid; ++l) {
                const Vector3 xyz{i / d / xyz_to_table, j / d / xyz_to_table, l / d / xyz_to_table};
                const Vector3 v = chromaroute::detail::multiply(scrgb_inverse, xyz);
                if (std::min({v[0], v[1], v[2]}) >= 0 && std::max({v[0], v[1], v[2]}) <= 1) {
                    rgb.insert(rgb.end(), {v[0], v[1], v[2]});
                }
            }
        }
    }
    return rgb;
}

std::vector<double> inverted(const std::vector<double>& inks)
{
    std::vector<double> v(inks.size());
    std::transform(inks.begin(), inks.end(), v.begin(), [](double x) { return 1 - x; });
    return v;
}

} // namespace

CR_TEST(cmyk_profiles_need_lcms2)
{
    const auto space = ColorSpace::fromIccProfile("cmyk-profile-check", naiveProfile(2, 2));
#if CHROMAROUTE_TEST_WITH_LCMS2
    CR_CHECK(space.has_value() && space->hasCmykProfile());
#else
    CR_CHECK(!space && space.error().code == ErrorCode::unsupported_profile &&
             space.error().message.find("lcms2") != std::string::npos);
    CR_CHECK(!ColorSpace::fromName("cmyk-profile-check").has_value());
#endif
    CR_CHECK(!ColorSpace::srgb().hasCmykProfile() && !ColorSpace::fromName("scRGB")->hasCmykProfile());
}

#if CHROMAROUTE_TEST_WITH_LCMS2

CR_TEST(cmyk_profile_spaces_are_srgb_with_their_profile)
{
    const ColorSpace& space = naiveSpace();
    const ColorSpace srgb = ColorSpace::srgb();
    CR_CHECK(space.hasCmykProfile() && space != srgb && space.name() == "naiveCMYK");
    const auto c = space.chromaticities();
    const auto s = srgb.chromaticities();
    CR_CHECK(c.white_x == s.white_x && c.white_y == s.white_y && c.red_x == s.red_x && c.red_y == s.red_y &&
             c.green_x == s.green_x && c.green_y == s.green_y && c.blue_x == s.blue_x && c.blue_y == s.blue_y);
    CR_CHECK(space.rgbToXyz() == srgb.rgbToXyz());
    for (std::size_t ch = 0; ch < 3; ++ch) {
        CR_CHECK(space.transfer(ch) == srgb.transfer(ch));
    }
    // Interned by the profile's bytes: another name finds the same space, and
    // bytes after the declared size do not belong to the profile.
    const auto again = ColorSpace::fromIccProfile("another-name", naiveProfile());
    CR_CHECK(again.has_value() && *again == space && again->name() == "naiveCMYK");
    auto padded = naiveProfile();
    padded.resize(padded.size() + 16, std::byte{0x5A});
    const auto with_padding = ColorSpace::fromIccProfile("", padded);
    CR_CHECK(with_padding.has_value() && *with_padding == space);
    const auto named = ColorSpace::fromName("naiveCMYK");
    CR_CHECK(named.has_value() && *named == space);
    const auto unnamed = ColorSpace::fromIccProfile("", naiveProfile(2, 9, true));
    CR_CHECK(unnamed.has_value() && *unnamed != space && unnamed->name().substr(0, 5) == "cmyk-");
    // Another TRC gives a space without the profile; sRGB's keeps it.
    CR_CHECK(!space.withTransfer(chromaroute::TransferFunction::linear()).hasCmykProfile());
    CR_CHECK(space.withTransfer(chromaroute::TransferFunction::srgb()) == space);

    // RGB and gray formats in it are sRGB's.
    std::vector<std::uint8_t> rgba(4 * 300);
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        rgba[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }
    CR_CHECK(convert<std::uint8_t>("R'G'B'A u8-naiveCMYK", "R'G'B'A u8", rgba) == rgba);
    CR_CHECK(convert<double>("R'G'B'A u8-naiveCMYK", "CIE Lab alpha double", rgba) ==
             convert<double>("R'G'B'A u8", "CIE Lab alpha double", rgba));
    CR_CHECK(convert<double>("Y' u8-naiveCMYK", "RGB double", rgba) == convert<double>("Y' u8", "RGB double", rgba));
}

CR_TEST(cmyk_profile_decodes_inks_through_its_tables)
{
    (void)naiveSpace();
    // Nodes: every combination of the inks 0, 1/4, ..., 1; then random inks.
    std::vector<double> inks;
    for (int c = 0; c <= 4; ++c) {
        for (int m = 0; m <= 4; ++m) {
            for (int y = 0; y <= 4; ++y) {
                for (int k = 0; k <= 4; ++k) {
                    inks.insert(inks.end(), {c / 4.0, m / 4.0, y / 4.0, k / 4.0});
                }
            }
        }
    }
    const std::size_t node_pixels = inks.size() / 4;
    std::mt19937 random(8);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int i = 0; i < 4 * 2000; ++i) {
        inks.push_back(unit(random));
    }
    const auto rgb = convert<double>("CMYK double-naiveCMYK", "RGB double-scRGB", inks);
    double worst_nodes = 0;
    double worst_between = 0;
    const bool sized = rgb.size() == inks.size() / 4 * 3;
    for (std::size_t p = 0; sized && p < inks.size() / 4; ++p) {
        const Vector3 expected = naiveRgb(inks[4 * p], inks[4 * p + 1], inks[4 * p + 2], inks[4 * p + 3]);
        for (std::size_t i = 0; i < 3; ++i) {
            const double error = std::fabs(rgb[3 * p + i] - expected[i]);
            if (p < node_pixels) {
                worst_nodes = worstOf(worst_nodes, error);
            } else {
                worst_between = worstOf(worst_between, error);
            }
        }
    }
    std::fprintf(cr_context.out(), "  CMYK profile decode: worst %.3g at nodes (bound %.3g), %.3g between (bound %.3g)\n",
                 worst_nodes, decodeBoundAtNodes(), worst_between, decodeBound());
    CR_CHECK(sized && worst_nodes <= decodeBoundAtNodes() && worst_between <= decodeBound());
    // The inverted form gives the same colors (these inks are exact either way).
    const std::vector<double> node_inks(inks.begin(), inks.begin() + static_cast<std::ptrdiff_t>(node_pixels * 4));
    CR_CHECK(convert<double>("cmyk double-naiveCMYK", "RGB double-scRGB", inverted(node_inks)) ==
             std::vector<double>(rgb.begin(), rgb.begin() + static_cast<std::ptrdiff_t>(node_pixels * 3)));
    // Through the swapped profile, which the naive formula does not give,
    // cyan acts as magenta and magenta as cyan.
    (void)swappedSpace();
    const auto swapped = convert<double>("CMYK double-swappedCMYK", "RGB double-scRGB", inks);
    double worst_swapped = 0;
    for (std::size_t p = 0; swapped.size() == rgb.size() && p < inks.size() / 4; ++p) {
        const Vector3 expected = naiveRgb(inks[4 * p + 1], inks[4 * p], inks[4 * p + 2], inks[4 * p + 3]);
        for (std::size_t i = 0; i < 3; ++i) {
            worst_swapped = worstOf(worst_swapped, std::fabs(swapped[3 * p + i] - expected[i]));
        }
    }
    CR_CHECK(swapped.size() == rgb.size() && worst_swapped <= decodeBound());
}

CR_TEST(cmyk_profile_encodes_rgb_through_its_tables)
{
    (void)naiveSpace();
    // At B2A0's nodes the table holds the formula's inks, rounded to uInt16.
    // lcms2 rounds the node's XYZ to 16 bits, up to a quarter of a percent of
    // a cell off it, and rounds its result to 16 bits; where the inks change
    // fast across the cell that moves them by up to 2 steps (measured 1).
    constexpr double bound = 2 * table_step;
    const std::vector<double> rgb = nodeColors(1);
    std::vector<double> expected;
    for (std::size_t p = 0; p < rgb.size() / 3; ++p) {
        for (double ink : naiveInks({rgb[3 * p], rgb[3 * p + 1], rgb[3 * p + 2]})) {
            expected.push_back(static_cast<double>(std::lround(ink * 65535.0)) / 65535.0);
        }
    }
    const auto inks = convert<double>("RGB double-scRGB", "CMYK double-naiveCMYK", rgb);
    double worst = 0;
    const bool sized = inks.size() == expected.size() && !expected.empty();
    for (std::size_t i = 0; sized && i < inks.size(); ++i) {
        worst = worstOf(worst, std::fabs(inks[i] - expected[i]));
    }
    std::fprintf(cr_context.out(), "  CMYK profile encode at %zu nodes: worst %.3g steps (bound 2)\n", rgb.size() / 3,
                 worst / table_step);
    CR_CHECK(sized && worst <= bound);
    // The inverted form agrees with it.
    CR_CHECK(convert<double>("RGB double-scRGB", "cmyk double-naiveCMYK", rgb) == inverted(inks));
    // The swapped profile's table holds the same inks with cyan and magenta
    // exchanged.
    (void)swappedSpace();
    const auto swapped = convert<double>("RGB double-scRGB", "CMYK double-swappedCMYK", rgb);
    double worst_swapped = 0;
    for (std::size_t p = 0; swapped.size() == expected.size() && p < expected.size() / 4; ++p) {
        const double want[4] = {expected[4 * p + 1], expected[4 * p], expected[4 * p + 2], expected[4 * p + 3]};
        for (std::size_t k = 0; k < 4; ++k) {
            worst_swapped = worstOf(worst_swapped, std::fabs(swapped[4 * p + k] - want[k]));
        }
    }
    CR_CHECK(swapped.size() == expected.size() && worst_swapped <= bound);
}

CR_TEST(cmyk_profiles_with_a_lab_connection_space)
{
    // Most CMYK profiles connect through Lab. At A2B0's nodes the stored Lab
    // is off by half an encoding step (L 100/65280, a and b 1/256) and lcms2's
    // 16-bit result by another half, a full step together. Through Lab to XYZ,
    // dX = Xn x 3 f^2 (dL / 116 + da / 500) with f <= 1 for colors in the
    // cube, likewise Y (dL only) and Z (db / 200); then M^-1 to linear RGB,
    // plus the two inks' 16-bit rounding, and 1e-5 for lcms2's D50 (the PCS
    // illuminant, 0.9642, 1, 0.8249) beside the engine's exact s15.16 one.
    const double dl = 100.0 / 65280.0;
    const double dab = 1.0 / 256.0;
    const double dxyz = std::max({lcms_d50[0] * 3 * (dl / 116 + dab / 500), 3 * dl / 116,
                                  lcms_d50[2] * 3 * (dl / 116 + dab / 200)});
    const double bound = inverseNorm() * dxyz + 2 * (0.5 / 65535.0) + 1e-5;
    const auto lab = ColorSpace::fromIccProfile("labCMYK", naiveCmykLabProfile(a2b_grid, 17).bytes());
    CR_CHECK(lab.has_value() && lab->hasCmykProfile());
    std::vector<double> inks;
    for (int c = 0; c <= 4; ++c) {
        for (int m = 0; m <= 4; ++m) {
            for (int y = 0; y <= 4; ++y) {
                for (int k = 0; k <= 4; ++k) {
                    inks.insert(inks.end(), {c / 4.0, m / 4.0, y / 4.0, k / 4.0});
                }
            }
        }
    }
    const auto rgb = convert<double>("CMYK double-labCMYK", "RGB double-scRGB", inks);
    double worst = 0;
    const bool sized = rgb.size() == inks.size() / 4 * 3;
    for (std::size_t p = 0; sized && p < inks.size() / 4; ++p) {
        const Vector3 expected = naiveRgb(inks[4 * p], inks[4 * p + 1], inks[4 * p + 2], inks[4 * p + 3]);
        for (std::size_t i = 0; i < 3; ++i) {
            worst = worstOf(worst, std::fabs(rgb[3 * p + i] - expected[i]));
        }
    }
    std::fprintf(cr_context.out(), "  CMYK profile with Lab: worst %.3g at nodes (bound %.3g)\n", worst, bound);
    CR_CHECK(sized && worst <= bound);
}

CR_TEST(cmyk_profile_black_point_is_compensated)
{
    // A profile whose darkest inks give 5% of white (linear RGB 0.05 + 0.95 x
    // the formula). Black point compensation maps that black to scRGB's, which
    // is 0, and scales the rest with it (A11), so the profile decodes to the
    // formula again: within the decoding bound, scaled by 1 / 0.95 as the
    // tables' steps are. Without it every color would keep the lift, up to
    // 0.05. (Encoding is not checked here: scRGB black then lands between
    // B2A0's first two nodes, where the coarse table interpolates.)
    constexpr double black = 0.05;
    const auto lifted = ColorSpace::fromIccProfile("liftedCMYK", naiveCmykProfile(a2b_grid, 9, false, black).bytes());
    CR_CHECK(lifted.has_value());
    std::mt19937 random(10);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::vector<double> inks{1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < 4 * 1000; ++i) {
        inks.push_back(unit(random));
    }
    const auto rgb = convert<double>("CMYK double-liftedCMYK", "RGB double-scRGB", inks);
    const double bound = decodeBound() / (1 - black) + 1e-6;
    double worst = 0;
    const bool sized = rgb.size() == inks.size() / 4 * 3;
    for (std::size_t p = 0; sized && p < inks.size() / 4; ++p) {
        const Vector3 expected = naiveRgb(inks[4 * p], inks[4 * p + 1], inks[4 * p + 2], inks[4 * p + 3]);
        for (std::size_t i = 0; i < 3; ++i) {
            worst = worstOf(worst, std::fabs(rgb[3 * p + i] - expected[i]));
        }
    }
    // The first inks are full ink, the profile's black: scRGB 0 within the bound.
    std::fprintf(cr_context.out(), "  CMYK black point compensation: worst %.3g (bound %.3g), black %.3g\n", worst,
                 bound, sized ? std::max({rgb[0], rgb[1], rgb[2]}) : 1.0);
    CR_CHECK(sized && worst <= bound);
}

CR_TEST(cmyk_profile_round_trips_colors_at_its_nodes)
{
    (void)naiveSpace();
    // Colors whose XYZ is a B2A0 node: their inks are within 2.5 steps of the
    // formula's (half a step stored, 2 from lcms2, as above), which moves each
    // RGB component by at most 2 x 2.5 steps (|dR / dc|, |dR / dk| <= 1); then
    // decoding adds its bound between A2B0's nodes. Between B2A0's nodes the
    // table bends across the planes where the largest channel changes, so
    // round trips there carry the table's own error, not the engine's.
    const double bound = decodeBound() + 5 * table_step;
    const std::vector<double> rgb = nodeColors(2);
    const auto back = convert<double>("CMYK double-naiveCMYK", "RGB double-scRGB",
                                      convert<double>("RGB double-scRGB", "CMYK double-naiveCMYK", rgb));
    double worst = 0;
    const bool sized = back.size() == rgb.size() && !rgb.empty();
    for (std::size_t i = 0; sized && i < rgb.size(); ++i) {
        worst = worstOf(worst, std::fabs(back[i] - rgb[i]));
    }
    std::fprintf(cr_context.out(), "  CMYK profile round trip at %zu nodes: worst %.3g (bound %.3g)\n", rgb.size() / 3,
                 worst, bound);
    CR_CHECK(sized && worst <= bound);
}

CR_TEST(cmyk_between_profiles_goes_through_both)
{
    (void)naiveSpace();
    (void)swappedSpace();
    std::vector<double> inks;
    for (int i = 0; i < 512; ++i) {
        const double a = ((i * 37) % 17) / 16.0;
        const double b = ((i * 11) % 13) / 12.0;
        const double k = ((i * 7) % 9) / 8.0 * 0.75;
        switch (i % 3) {
            case 0: inks.insert(inks.end(), {0.0, a, b, k}); break;
            case 1: inks.insert(inks.end(), {a, 0.0, b, k}); break;
            default: inks.insert(inks.end(), {a, b, 0.0, k}); break;
        }
    }
    inks.insert(inks.end(), {0.5, 0.0, 0.25, 0.25});
    // The swapped profile's B2A0 is the first's with cyan and magenta
    // exchanged, node for node, so going from the first profile to the second
    // is the first's own round trip with the two inks exchanged. Through scRGB
    // the same tables are met, with a double matrix and its inverse between
    // them; lcms2 rounds both to the same 16-bit inputs (measured: no
    // difference), and one step is allowed.
    const auto direct = convert<double>("CMYK double-naiveCMYK", "CMYK double-swappedCMYK", inks);
    const auto staged = convert<double>("RGB double-scRGB", "CMYK double-swappedCMYK",
                                        convert<double>("CMYK double-naiveCMYK", "RGB double-scRGB", inks));
    const auto own = convert<double>("RGB double-scRGB", "CMYK double-naiveCMYK",
                                     convert<double>("CMYK double-naiveCMYK", "RGB double-scRGB", inks));
    double worst_staged = 0;
    double worst_swap = 0;
    const std::size_t pixels = inks.size() / 4;
    const bool sized = direct.size() == inks.size() && staged.size() == inks.size() && own.size() == inks.size();
    for (std::size_t p = 0; sized && p < pixels; ++p) {
        const double swapped_own[4] = {own[4 * p + 1], own[4 * p], own[4 * p + 2], own[4 * p + 3]};
        for (std::size_t k = 0; k < 4; ++k) {
            worst_staged = worstOf(worst_staged, std::fabs(direct[4 * p + k] - staged[4 * p + k]));
            worst_swap = worstOf(worst_swap, std::fabs(direct[4 * p + k] - swapped_own[k]));
        }
    }
    std::fprintf(cr_context.out(), "  CMYK between profiles: %.3g steps from staged, %.3g from the swapped round trip\n",
                 worst_staged / table_step, worst_swap / table_step);
    CR_CHECK(sized && worst_staged <= table_step && worst_swap <= table_step);
    // Not the same inks: (0.5, 0, 0.25, 0.25) comes out with cyan nearer 0 and
    // magenta nearer 0.5, the inks exchanged.
    CR_CHECK(sized && direct[4 * (pixels - 1)] < 0.25 && direct[4 * (pixels - 1) + 1] > 0.25);
    // Associated inks and the inverted form, between the two profiles.
    std::vector<double> with_alpha;
    for (std::size_t p = 0; p < pixels; ++p) {
        for (std::size_t k = 0; k < 4; ++k) {
            with_alpha.push_back(inks[4 * p + k] * 0.5);
        }
        with_alpha.push_back(0.5);
    }
    const auto associated = convert<double>("CaMaYaKaA double-naiveCMYK", "cmykA double-swappedCMYK", with_alpha);
    bool same = associated.size() == pixels * 5;
    for (std::size_t p = 0; same && p < pixels; ++p) {
        for (std::size_t k = 0; k < 4; ++k) {
            same = same && associated[5 * p + k] == 1 - direct[4 * p + k];
        }
        same = same && associated[5 * p + 4] == 0.5;
    }
    CR_CHECK(same);
    // One profile, or a profile and naive CMYK: the same inks, in any form.
    CR_CHECK(convert<double>("CMYK double-naiveCMYK", "CMYK double", inks) == inks);
    CR_CHECK(convert<double>("CMYK double", "CMYK double-naiveCMYK", inks) == inks);
    CR_CHECK(convert<double>("CMYK double-naiveCMYK", "cmyk double-naiveCMYK", inks) == inverted(inks));
}

CR_TEST(cmyk_profile_alpha_association_and_non_finite_values)
{
    (void)naiveSpace();
    // Associated inks are un-associated before the profile and associated
    // after it; alpha is kept. Alphas of 1/2, 1 and 1/4 make the division
    // exact, so the profile meets the same inks.
    const std::vector<double> straight{0.25, 0.5, 0.0, 0.25, 0.5, 0.0, 0.0, 0.0, 1.0, 1.0, 0.5, 0.25, 0.75, 0.0, 0.25};
    std::vector<double> associated(straight);
    for (std::size_t p = 0; p < 3; ++p) {
        for (std::size_t k = 0; k < 4; ++k) {
            associated[5 * p + k] *= straight[5 * p + 4];
        }
    }
    const auto from_straight = convert<double>("CMYKA double-naiveCMYK", "RGBA double-scRGB", straight);
    const auto from_associated = convert<double>("CaMaYaKaA double-naiveCMYK", "RGBA double-scRGB", associated);
    CR_CHECK(from_straight.size() == 12 && from_straight == from_associated);
    for (std::size_t p = 0; p < 3 && from_straight.size() == 12; ++p) {
        CR_CHECK(from_straight[4 * p + 3] == straight[5 * p + 4]);
    }
    const auto back_straight = convert<double>("RGBA double-scRGB", "CMYKA double-naiveCMYK", from_straight);
    const auto back_associated = convert<double>("RGBA double-scRGB", "CaMaYaKaA double-naiveCMYK", from_straight);
    bool associates = back_straight.size() == 15 && back_associated.size() == 15;
    for (std::size_t p = 0; associates && p < 3; ++p) {
        for (std::size_t k = 0; k < 4; ++k) {
            associates = associates && back_associated[5 * p + k] == back_straight[5 * p + k] * straight[5 * p + 4];
        }
        associates = associates && back_associated[5 * p + 4] == straight[5 * p + 4];
    }
    CR_CHECK(associates);

    // A NaN or infinite component gives NaN in every color, both ways; alpha is kept.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const auto rgb = convert<double>("CMYKA double-naiveCMYK", "RGBA double-scRGB",
                                     std::vector<double>{nan, 0, 0, 0, 1, 0, inf, 0, 0, 1, 0.5, 0.5, 0.5, -inf, 1});
    bool all_nan = rgb.size() == 12;
    for (std::size_t p = 0; all_nan && p < 3; ++p) {
        all_nan = std::isnan(rgb[4 * p]) && std::isnan(rgb[4 * p + 1]) && std::isnan(rgb[4 * p + 2]) &&
                  rgb[4 * p + 3] == 1;
    }
    CR_CHECK(all_nan);
    const auto inks = convert<double>("RGBA double-scRGB", "CMYKA double-naiveCMYK",
                                      std::vector<double>{nan, 0.5, 0.5, 1, 0.5, -inf, 0.5, 1, 0.5, 0.5, inf, 1});
    all_nan = inks.size() == 15;
    for (std::size_t p = 0; all_nan && p < 3; ++p) {
        for (std::size_t k = 0; k < 4; ++k) {
            all_nan = all_nan && std::isnan(inks[5 * p + k]);
        }
        all_nan = all_nan && inks[5 * p + 4] == 1;
    }
    CR_CHECK(all_nan);
}

CR_TEST(cmyk_profiles_with_problems_are_rejected)
{
    struct Case {
        const char* what;
        std::vector<std::byte> bytes;
        ErrorCode code;
    };
    const auto b2a = [](IccWriter& w) {
        w.lut16("B2A0", 3, 4, 2, [](const std::vector<double>&) { return std::vector<double>{0, 0, 0, 1}; });
    };
    IccWriter only_b2a(2, "CMYK", "XYZ ", "prtr");
    only_b2a.xyz("wtpt", 0.9642, 1.0, 0.8249);
    b2a(only_b2a);
    IccWriter wrong_type(2, "CMYK", "XYZ ", "prtr");
    wrong_type.xyz("wtpt", 0.9642, 1.0, 0.8249).raw("A2B0", std::vector<std::byte>(64));
    b2a(wrong_type);
    IccWriter link(2, "CMYK", "XYZ ", "link");
    IccWriter abstract(2, "CMYK", "XYZ ", "abst");
    IccWriter unknown_pcs(2, "CMYK", "abcd", "prtr");
    const std::vector<Case> cases = {
        {"no B2A0 (an input-only profile)", naiveProfile(2, 0), ErrorCode::unsupported_profile},
        {"no A2B0", only_b2a.bytes(), ErrorCode::unsupported_profile},
        {"A2B0 of no known type", wrong_type.bytes(), ErrorCode::invalid_profile},
        {"device link", link.bytes(), ErrorCode::unsupported_profile},
        {"abstract", abstract.bytes(), ErrorCode::unsupported_profile},
        {"unknown connection space", unknown_pcs.bytes(), ErrorCode::invalid_profile},
    };
    int index = 0;
    for (const Case& c : cases) {
        // A rejected profile registers no space under the name it was given.
        const std::string name = "rejected-cmyk-" + std::to_string(index++);
        const auto space = ColorSpace::fromIccProfile(name, c.bytes);
        const bool ok = !space && space.error().code == c.code && !space.error().message.empty() &&
                        !ColorSpace::fromName(name).has_value();
        if (!ok) {
            std::fprintf(cr_context.out(), "  %s: %s\n", c.what, space ? "accepted" : space.error().message.c_str());
        }
        CR_CHECK(ok);
    }
}

CR_TEST(cmyk_profile_corruptions_never_misbehave)
{
    // Every truncation of a small CMYK profile, with its declared size made to
    // match, and 2,000 random corruptions of 1 to 8 bytes: each gives a space
    // or a profile error, never anything else. lcms2 reads what the library's
    // reader passes on (the header and tag table are checked first).
    const auto valid = naiveProfile(2, 3);
    int bad = 0;
    for (std::size_t size = 0; size < valid.size(); ++size) {
        auto cut = std::vector<std::byte>(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(size));
        if (size >= 4) {
            IccWriter::set32(cut, 0, static_cast<std::uint32_t>(size));
        }
        const auto space = ColorSpace::fromIccProfile("", cut);
        bad += space || (space.error().code != ErrorCode::invalid_profile &&
                         space.error().code != ErrorCode::unsupported_profile);
    }
    std::mt19937 random(2026);
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
            // A corrupted profile that opens converts without misbehaving.
            const std::string format = "CMYK u8-" + std::string(space->name());
            bad += convert<std::uint8_t>(format.c_str(), "R'G'B' u8", std::vector<std::uint8_t>{0, 64, 128, 255})
                       .size() != 3;
        } else if (space.error().code == ErrorCode::invalid_profile ||
                   space.error().code == ErrorCode::unsupported_profile) {
            ++errors;
        } else {
            ++bad;
        }
    }
    std::fprintf(cr_context.out(), "  corrupted CMYK profiles: %d accepted, %d rejected\n", spaces, errors);
    CR_CHECK(bad == 0);
}

CR_TEST(cmyk_profile_converters_are_thread_safe)
{
    (void)naiveSpace();
    // One converter each way, shared by 8 threads; lcms2's transforms are
    // created without its cache, which is what makes that safe.
    constexpr std::size_t pixels = 4096;
    std::vector<std::uint8_t> inks(pixels * 4);
    for (std::size_t i = 0; i < inks.size(); ++i) {
        inks[i] = static_cast<std::uint8_t>((i * 29 + 7) & 0xFF);
    }
    const Converter decode =
        *Converter::create(*PixelFormat::fromName("CMYK u8-naiveCMYK"), *PixelFormat::fromName("R'G'B' u8"));
    const Converter encode =
        *Converter::create(*PixelFormat::fromName("R'G'B' u8"), *PixelFormat::fromName("CMYK u8-naiveCMYK"));
    std::vector<std::uint8_t> rgb(pixels * 3);
    std::vector<std::uint8_t> back(pixels * 4);
    CR_CHECK(decode.process(std::as_bytes(std::span(inks)), std::as_writable_bytes(std::span(rgb)), pixels).has_value());
    CR_CHECK(encode.process(std::as_bytes(std::span(rgb)), std::as_writable_bytes(std::span(back)), pixels).has_value());
    std::vector<int> same(8, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            bool ok = true;
            for (int round = 0; round < 20; ++round) {
                std::vector<std::uint8_t> r(pixels * 3);
                std::vector<std::uint8_t> b(pixels * 4);
                ok = ok && decode.process(std::as_bytes(std::span(inks)), std::as_writable_bytes(std::span(r)), pixels);
                ok = ok && encode.process(std::as_bytes(std::span(r)), std::as_writable_bytes(std::span(b)), pixels);
                ok = ok && r == rgb && b == back;
            }
            same[static_cast<std::size_t>(t)] = ok ? 1 : 0;
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CR_CHECK(std::count(same.begin(), same.end(), 1) == 8);
}

#endif
