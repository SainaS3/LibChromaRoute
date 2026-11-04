// ICC profiles (Milestone 8a): RGB and gray matrix/TRC profiles read by
// LibChromaRoute itself (decision D1), the curve tags they carry, and the
// rejection of malformed, truncated, corrupted and unsupported profiles.

#include "support/conversion_checks.hpp"
#include "support/icc_writer.hpp"
#include "support/test_harness.hpp"

#include <chromaroute/chromaroute.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using chromaroute::ColorSpace;
using chromaroute::ErrorCode;
using chromaroute::TransferFunction;
using chromaroute::test::IccWriter;
using chromaroute::test::s15;
using namespace chromaroute::test;

namespace {

using Kind = TransferFunction::Kind;

// sRGB's curve as an ICC parametric type 3, in s15Fixed16.
const std::vector<std::int32_t> srgb_para{s15(2.4), s15(1 / 1.055), s15(0.055 / 1.055), s15(1 / 12.92), s15(0.04045)};

void srgbCurve(IccWriter& w, const char* sig)
{
    w.para(sig, 3, srgb_para);
}

std::vector<std::byte> srgbLikeProfile()
{
    return IccWriter::rgb(srgbCurve).bytes();
}

ErrorCode codeOf(std::string_view name, const std::vector<std::byte>& bytes)
{
    const auto space = ColorSpace::fromIccProfile(name, bytes);
    return space ? static_cast<ErrorCode>(-1) : space.error().code;
}

} // namespace

CR_TEST(icc_matrix_trc_profiles_give_their_matrix_and_curves)
{
    const auto bytes = srgbLikeProfile();
    const auto space = ColorSpace::fromIccProfile("icc-srgb-like", bytes);
    CR_CHECK(space.has_value());
    if (!space) {
        return;
    }
    // The colorants are the matrix's columns, decoded exactly from s15Fixed16.
    const double r[3] = {s15(0.4360747) / 65536.0, s15(0.2225045) / 65536.0, s15(0.0139322) / 65536.0};
    const double g[3] = {s15(0.3850649) / 65536.0, s15(0.7168786) / 65536.0, s15(0.0971045) / 65536.0};
    const double b[3] = {s15(0.1430804) / 65536.0, s15(0.0606169) / 65536.0, s15(0.7141733) / 65536.0};
    bool exact = true;
    for (std::size_t row = 0; row < 3; ++row) {
        exact = exact && space->rgbToXyz()[row][0] == r[row] && space->rgbToXyz()[row][1] == g[row] &&
                space->rgbToXyz()[row][2] == b[row];
    }
    CR_CHECK(exact);
    CR_CHECK(space->name() == "icc-srgb-like");
    for (std::size_t c = 0; c < 3; ++c) {
        const TransferFunction t = space->transfer(c);
        const auto p = t.parametricCurve();
        CR_CHECK(t.kind() == Kind::parametric && p.type == 3 && p.g == srgb_para[0] / 65536.0 &&
                 p.a == srgb_para[1] / 65536.0 && p.b == srgb_para[2] / 65536.0 && p.c == srgb_para[3] / 65536.0 &&
                 p.d == srgb_para[4] / 65536.0 && p.e == 0.0 && p.f == 0.0);
    }
    // It is the space fromRgbToXyz makes from the same data, interned once.
    const auto curve = TransferFunction::parametric({3, srgb_para[0] / 65536.0, srgb_para[1] / 65536.0,
                                                     srgb_para[2] / 65536.0, srgb_para[3] / 65536.0,
                                                     srgb_para[4] / 65536.0, 0, 0});
    const auto same = ColorSpace::fromRgbToXyz("other-name", {s15(0.9642) / 65536.0, 1.0, s15(0.8249) / 65536.0},
                                               space->rgbToXyz(), *curve, *curve, *curve);
    CR_CHECK(same.has_value() && *same == *space);
    CR_CHECK(*ColorSpace::fromIccProfile("", bytes) == *space);
    // s15Fixed16 numbers are signed: wide-gamut colorants and curve offsets can
    // be negative.
    const auto wide = IccWriter()
                          .xyz("rXYZ", 0.7977, 0.2880, -0.0001)
                          .xyz("gXYZ", 0.1352, 0.7119, 0.0000)
                          .xyz("bXYZ", 0.0313, 0.0001, 0.8251)
                          .xyz("wtpt", 0.9642, 1.0, 0.8249)
                          .para("rTRC", 4, {s15(2.2), s15(1.0), s15(-0.05), s15(0.5), s15(0.1), s15(-0.01), s15(-0.002)})
                          .para("gTRC", 3, srgb_para)
                          .para("bTRC", 3, srgb_para)
                          .bytes();
    const auto signed_space = ColorSpace::fromIccProfile("", wide);
    CR_CHECK(signed_space.has_value() && signed_space->rgbToXyz()[2][0] == s15(-0.0001) / 65536.0 &&
             signed_space->rgbToXyz()[2][0] < 0.0);
    if (signed_space) {
        const auto p = signed_space->transfer(0).parametricCurve();
        CR_CHECK(p.type == 4 && p.b == s15(-0.05) / 65536.0 && p.e == s15(-0.01) / 65536.0 &&
                 p.f == s15(-0.002) / 65536.0 && p.f < 0.0);
    }
    // Formats in it convert through its matrix (N9), like any matrix space.
    const auto lab = convert<float>(("R'G'B' u8-" + std::string(space->name())).c_str(), "CIE Lab float",
                                    std::vector<std::uint8_t>{255, 255, 255, 0, 0, 0});
    CR_CHECK(lab.size() == 6 && std::fabs(lab[0] - 100.0f) < 0.01f && lab[3] == 0.0f);
}

CR_TEST(icc_curve_tags_become_their_transfer_kinds)
{
    struct Case {
        std::vector<std::uint16_t> entries;
        Kind kind;
    };
    std::vector<std::uint16_t> table(1024);
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<std::uint16_t>(std::lround(65535.0 * std::pow(i / 1023.0, 1.8)));
    }
    const Case cases[] = {{{}, Kind::linear}, {{563}, Kind::gamma}, {table, Kind::sampled}};
    for (const Case& c : cases) {
        const auto bytes = IccWriter::rgb([&](IccWriter& w, const char* sig) { w.curve(sig, c.entries); }, 2).bytes();
        const auto space = ColorSpace::fromIccProfile("", bytes);
        CR_CHECK(space.has_value() && space->transfer(0).kind() == c.kind);
        if (!space) {
            continue;
        }
        if (c.kind == Kind::gamma) {
            CR_CHECK(space->transfer(0).gammaValue() == 563 / 256.0); // u8Fixed8
        }
        if (c.kind == Kind::sampled) {
            const auto samples = space->transfer(0).samples();
            bool exact = samples.size() == table.size();
            for (std::size_t i = 0; exact && i < table.size(); ++i) {
                exact = samples[i] == table[i] / 65535.0;
            }
            CR_CHECK(exact);
        }
    }
    // Parametric type 0 is a pure gamma.
    const auto type0 = IccWriter::rgb([](IccWriter& w, const char* sig) { w.para(sig, 0, {s15(2.2)}); }).bytes();
    const auto space0 = ColorSpace::fromIccProfile("", type0);
    CR_CHECK(space0.has_value() && space0->transfer(0).kind() == Kind::gamma &&
             space0->transfer(0).gammaValue() == s15(2.2) / 65536.0);
}

CR_TEST(icc_gray_profiles_use_srgb_primaries_and_their_curve)
{
    IccWriter w(4, "GRAY");
    w.xyz("wtpt", 0.9642, 1.0, 0.8249).para("kTRC", 3, srgb_para);
    const auto space = ColorSpace::fromIccProfile("icc-gray", w.bytes());
    CR_CHECK(space.has_value());
    if (!space) {
        return;
    }
    const auto srgb = ColorSpace::srgb().chromaticities();
    const auto c = space->chromaticities();
    CR_CHECK(c.white_x == srgb.white_x && c.red_x == srgb.red_x && c.green_y == srgb.green_y && c.blue_y == srgb.blue_y);
    CR_CHECK(space->transfer(0) == space->transfer(1) && space->transfer(1) == space->transfer(2) &&
             space->transfer(0).kind() == Kind::parametric);
    // Gray values decode with the curve and stay exactly neutral.
    const auto y = convert<double>("Y' double-icc-gray", "Y double", std::vector<double>{0.5});
    const double expected = std::pow(srgb_para[1] / 65536.0 * 0.5 + srgb_para[2] / 65536.0, srgb_para[0] / 65536.0);
    CR_CHECK(y.size() == 1 && std::fabs(y[0] - expected) < 1e-16);
    const auto lab = convert<double>("Y' double-icc-gray", "CIE Lab double", std::vector<double>{0.5});
    CR_CHECK(lab.size() == 3 && lab[1] == 0.0 && lab[2] == 0.0);
}

CR_TEST(icc_profiles_with_problems_are_rejected)
{
    const auto valid = srgbLikeProfile();
    CR_CHECK(codeOf("", valid) == static_cast<ErrorCode>(-1));
    const auto patched = [&](std::size_t at, std::uint32_t value) {
        auto b = valid;
        IccWriter::set32(b, at, value);
        return b;
    };
    struct Case {
        const char* what;
        std::vector<std::byte> bytes;
        ErrorCode code;
    };
    const auto sig = [](const char* s) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24 |
               static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16 |
               static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8 |
               static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
    };
    std::vector<std::uint16_t> long_table(65537, 0);
    std::vector<Case> cases = {
        {"empty", {}, ErrorCode::invalid_profile},
        {"131 bytes", std::vector<std::byte>(valid.begin(), valid.begin() + 131), ErrorCode::invalid_profile},
        {"declared size larger than given", patched(0, static_cast<std::uint32_t>(valid.size() + 1)),
         ErrorCode::invalid_profile},
        {"declared size below the header", patched(0, 100), ErrorCode::invalid_profile},
        {"no acsp", patched(36, sig("acsq")), ErrorCode::invalid_profile},
        {"tag count beyond the profile", patched(128, 0x7FFFFFFF), ErrorCode::invalid_profile},
        {"tag offset beyond the profile", patched(132 + 4, static_cast<std::uint32_t>(valid.size())),
         ErrorCode::invalid_profile},
        {"tag offset that overflows", patched(132 + 4, 0xFFFFFFF0u), ErrorCode::invalid_profile},
        {"tag size beyond the profile", patched(132 + 8, 0x7FFFFFFF), ErrorCode::invalid_profile},
        {"version 5", IccWriter(5).bytes(), ErrorCode::unsupported_profile},
        {"device link", IccWriter(4, "RGB ", "XYZ ", "link").bytes(), ErrorCode::unsupported_profile},
        {"CMYK", IccWriter(4, "CMYK").bytes(), ErrorCode::unsupported_profile},
        {"Lab connection space", IccWriter(4, "RGB ", "Lab ").bytes(), ErrorCode::unsupported_profile},
        {"unknown connection space", IccWriter(4, "RGB ", "abcd").bytes(), ErrorCode::invalid_profile},
        // With lcms2 the profile goes to it (A12), which cannot read the table.
        {"lookup tables only", IccWriter().raw("A2B0", std::vector<std::byte>(32)).bytes(),
#if CHROMAROUTE_TEST_WITH_LCMS2
         ErrorCode::invalid_profile},
#else
         ErrorCode::unsupported_profile},
#endif
        {"no tags", IccWriter().bytes(), ErrorCode::invalid_profile},
        {"no white point",
         IccWriter().xyz("rXYZ", 0.4, 0.2, 0.0).xyz("gXYZ", 0.4, 0.7, 0.1).xyz("bXYZ", 0.1, 0.1, 0.7)
             .para("rTRC", 3, srgb_para).para("gTRC", 3, srgb_para).para("bTRC", 3, srgb_para).bytes(),
         ErrorCode::invalid_profile},
        {"colorant of the wrong type",
         IccWriter().raw("rXYZ", std::vector<std::byte>(20)).xyz("gXYZ", 0.4, 0.7, 0.1).xyz("bXYZ", 0.1, 0.1, 0.7)
             .xyz("wtpt", 0.9642, 1.0, 0.8249).para("rTRC", 3, srgb_para).para("gTRC", 3, srgb_para)
             .para("bTRC", 3, srgb_para).bytes(),
         ErrorCode::invalid_profile},
        {"curve of an unknown type",
         IccWriter::rgb([](IccWriter& w, const char* s) { w.raw(s, std::vector<std::byte>(16)); }).bytes(),
         ErrorCode::invalid_profile},
        {"parametric type 5",
         IccWriter::rgb([](IccWriter& w, const char* s) { w.para(s, 5, {s15(2.2)}); }).bytes(), ErrorCode::invalid_profile},
        {"parametric curve too short",
         IccWriter::rgb([](IccWriter& w, const char* s) { w.para(s, 3, {s15(2.4), s15(1.0)}); }).bytes(),
         ErrorCode::invalid_profile},
        {"parametric a of 0",
         IccWriter::rgb([](IccWriter& w, const char* s) { w.para(s, 1, {s15(2.4), 0, 0}); }).bytes(),
         ErrorCode::invalid_profile},
        {"gamma 0", IccWriter::rgb([](IccWriter& w, const char* s) { w.curve(s, {0}); }).bytes(),
         ErrorCode::invalid_profile},
        {"curve with more than 65536 entries",
         IccWriter::rgb([&](IccWriter& w, const char* s) { w.curve(s, long_table); }).bytes(), ErrorCode::invalid_profile},
        {"singular matrix",
         IccWriter().xyz("rXYZ", 0.4, 0.2, 0.0).xyz("gXYZ", 0.4, 0.2, 0.0).xyz("bXYZ", 0.1, 0.1, 0.7)
             .xyz("wtpt", 0.9642, 1.0, 0.8249).para("rTRC", 3, srgb_para).para("gTRC", 3, srgb_para)
             .para("bTRC", 3, srgb_para).bytes(),
         ErrorCode::invalid_profile},
        {"gray profile without kTRC", IccWriter(4, "GRAY").xyz("wtpt", 0.9642, 1.0, 0.8249).bytes(),
         ErrorCode::invalid_profile},
    };
    // A curve whose entry count exceeds its tag.
    const auto overlong = IccWriter::rgb([](IccWriter& w, const char* s) { w.curve(s, {1000, 2000}); }).bytes();
    auto overlong_count = overlong;
    // The rTRC tag is the fifth: its count sits 8 bytes into its data.
    const std::size_t rtrc = 132 + 12 * 4;
    const std::size_t rtrc_offset = std::to_integer<std::size_t>(overlong[rtrc + 4]) << 24 |
                                    std::to_integer<std::size_t>(overlong[rtrc + 5]) << 16 |
                                    std::to_integer<std::size_t>(overlong[rtrc + 6]) << 8 |
                                    std::to_integer<std::size_t>(overlong[rtrc + 7]);
    IccWriter::set32(overlong_count, rtrc_offset + 8, 3);
    cases.push_back({"curve count beyond its tag", overlong_count, ErrorCode::invalid_profile});
    for (const Case& c : cases) {
        const auto space = ColorSpace::fromIccProfile("", c.bytes);
        const bool ok = !space && space.error().code == c.code && !space.error().message.empty();
        if (!ok) {
            std::fprintf(cr_context.out(), "  %s: %s\n", c.what, space ? "accepted" : space.error().message.c_str());
        }
        CR_CHECK(ok);
    }
}

CR_TEST(icc_profile_signatures_are_checked_against_icc1)
{
    // ICC.1 defines seven profile classes and a list of data color spaces. A
    // defined signature that is not read is unsupported; any other is malformed.
    const auto valid = srgbLikeProfile();
    const auto withSignature = [&](std::size_t at, const char* sig) {
        auto b = valid;
        for (std::size_t i = 0; i < 4; ++i) {
            b[at + i] = std::byte{static_cast<unsigned char>(sig[i])};
        }
        return b;
    };
    // Classes (header byte 12). ICC.1 defines three-component matrix/TRC profiles
    // for the input and display classes only; its output and color space
    // profiles are built on lookup tables.
    for (const char* read : {"scnr", "mntr"}) {
        CR_CHECK(codeOf("", withSignature(12, read)) == static_cast<ErrorCode>(-1));
    }
    for (const char* defined : {"prtr", "spac", "link", "abst", "nmcl"}) {
        CR_CHECK(codeOf("", withSignature(12, defined)) == ErrorCode::unsupported_profile);
    }
    for (const char* unknown : {"abcd", "MNTR", "mnt ", "\0\0\0\0"}) {
        CR_CHECK(codeOf("", withSignature(12, unknown)) == ErrorCode::invalid_profile);
    }
    // Monochrome profiles are defined for the output class too.
    const auto grayProfile = [](const char* device_class) {
        IccWriter gray(4, "GRAY", "XYZ ", device_class);
        gray.xyz("wtpt", 0.9642, 1.0, 0.8249).para("kTRC", 3, srgb_para);
        return gray.bytes();
    };
    for (const char* read : {"scnr", "mntr", "prtr"}) {
        CR_CHECK(codeOf("", grayProfile(read)) == static_cast<ErrorCode>(-1));
    }
    for (const char* defined : {"spac", "link", "abst", "nmcl"}) {
        CR_CHECK(codeOf("", grayProfile(defined)) == ErrorCode::unsupported_profile);
    }
    CR_CHECK(codeOf("", grayProfile("abcd")) == ErrorCode::invalid_profile);
    // Data color spaces (header byte 16), 2 to 15 colorants included.
    for (const char* defined : {"XYZ ", "Lab ", "Luv ", "YCbr", "Yxy ", "HSV ", "HLS ", "CMYK", "CMY ", "2CLR", "9CLR",
                                "ACLR", "FCLR"}) {
        CR_CHECK(codeOf("", withSignature(16, defined)) == ErrorCode::unsupported_profile);
    }
    for (const char* unknown : {"abcd", "rgb ", "Gray", "1CLR", "GCLR", "aCLR", "3CMY", "ACL ", "\0\0\0\0"}) {
        CR_CHECK(codeOf("", withSignature(16, unknown)) == ErrorCode::invalid_profile);
    }
}

CR_TEST(rejected_icc_profiles_leave_no_curves_behind)
{
    // A profile is checked whole before any of its curves or its space is
    // created, so a rejected profile registers nothing. The names follow
    // TransferFunction::name(): "para<type>:<%.17g parameters>" and
    // "table<n>:<64-bit FNV-1a of the samples' bits>".
    const auto paraName = [](int type, const std::vector<std::int32_t>& raw) {
        double p[7] = {};
        for (std::size_t i = 0; i < raw.size(); ++i) {
            p[i] = raw[i] / 65536.0;
        }
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "para%d:%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", type, p[0], p[1], p[2],
                      p[3], p[4], p[5], p[6]);
        return std::string(buffer);
    };
    const auto tableName = [](const std::vector<std::uint16_t>& entries) {
        std::uint64_t hash = 0xcbf29ce484222325u;
        for (std::uint16_t e : entries) {
            const auto bits = std::bit_cast<std::uint64_t>(e / 65535.0);
            for (int byte = 0; byte < 8; ++byte) {
                hash = (hash ^ ((bits >> (8 * byte)) & 0xFF)) * 0x100000001b3u;
            }
        }
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "table%zu:%016llx", entries.size(), static_cast<unsigned long long>(hash));
        return std::string(buffer);
    };
    const std::vector<std::int32_t> unique_para{s15(2.123), s15(0.987), s15(0.013)};
    const std::vector<std::uint16_t> unique_table{0, 7, 999, 31111, 65535};
    const auto profile = [&](double red_y, bool bad_green) {
        IccWriter w;
        w.xyz("rXYZ", 0.4, red_y, 0.0).xyz("gXYZ", 0.4, 0.2, 0.0).xyz("bXYZ", 0.1, 0.1, 0.7).xyz("wtpt", 0.9642, 1.0, 0.8249);
        w.curve("rTRC", unique_table);
        if (bad_green) {
            w.para("gTRC", 1, {s15(2.4), 0, 0}); // a = 0
        } else {
            w.para("gTRC", 1, unique_para);
        }
        w.para("bTRC", 1, unique_para);
        return w.bytes();
    };
    // A singular matrix (rXYZ = gXYZ), then a valid matrix with an invalid
    // green curve: both rejected, neither curve registered.
    CR_CHECK(codeOf("", profile(0.2, false)) == ErrorCode::invalid_profile);
    CR_CHECK(codeOf("", profile(0.25, true)) == ErrorCode::invalid_profile);
    CR_CHECK(!TransferFunction::fromName(paraName(1, unique_para)).has_value());
    CR_CHECK(!TransferFunction::fromName(tableName(unique_table)).has_value());
    // The same curves in a valid profile are registered under those names.
    CR_CHECK(ColorSpace::fromIccProfile("", profile(0.25, false)).has_value());
    CR_CHECK(TransferFunction::fromName(paraName(1, unique_para)).has_value());
    CR_CHECK(TransferFunction::fromName(tableName(unique_table)).has_value());
}

CR_TEST(icc_truncated_and_corrupted_profiles_never_misbehave)
{
    // Every truncation, with the declared size made to match, and many random
    // byte corruptions: each gives a space or a profile error, never anything
    // else (run under the checked Debug library too).
    const auto valid = srgbLikeProfile();
    int bad = 0;
    for (std::size_t length = 0; length <= valid.size(); ++length) {
        std::vector<std::byte> cut(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        if (length >= 4) {
            IccWriter::set32(cut, 0, static_cast<std::uint32_t>(length));
        }
        const auto space = ColorSpace::fromIccProfile("", cut);
        bad += length < valid.size() && space.has_value(); // the table and tags no longer fit
        bad += !space && space.error().code != ErrorCode::invalid_profile;
    }
    CR_CHECK(bad == 0);
    std::uint32_t state = 0x9E3779B9u;
    int accepted = 0, rejected = 0, other = 0;
    for (int round = 0; round < 20000; ++round) {
        auto b = valid;
        for (int hits = 0; hits < 1 + round % 4; ++hits) {
            state = state * 1664525u + 1013904223u;
            const std::size_t at = 4 + (state >> 8) % (b.size() - 4); // keep the declared size
            state = state * 1664525u + 1013904223u;
            b[at] = std::byte{static_cast<unsigned char>(state >> 24)};
        }
        const auto space = ColorSpace::fromIccProfile("", b);
        if (space) {
            ++accepted;
        } else if (space.error().code == ErrorCode::invalid_profile ||
                   space.error().code == ErrorCode::unsupported_profile) {
            ++rejected;
        } else {
            ++other;
        }
    }
    std::fprintf(cr_context.out(), "  corrupted profiles: %d accepted, %d rejected\n", accepted, rejected);
    CR_CHECK(other == 0 && accepted > 0 && rejected > 0);
}
