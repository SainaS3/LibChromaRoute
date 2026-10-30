#include "core/descriptors.hpp"

#include "engine/color_math.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chromaroute::detail {
namespace {

constexpr TypeDescriptor types[] = {
    {"u8", ComponentType::u8, 1},      {"u16", ComponentType::u16, 2},   {"u32", ComponentType::u32, 4},
    {"half", ComponentType::f16, 2},   {"float", ComponentType::f32, 4}, {"double", ComponentType::f64, 8},
};

// Component names per encoding and alpha style: [encoding][associated][R, G, B, Y].
constexpr ComponentDescriptor color_components[3][2][4] = {
    {{{"R"}, {"G"}, {"B"}, {"Y"}}, {{"Ra"}, {"Ga"}, {"Ba"}, {"Ya"}}},
    {{{"R'"}, {"G'"}, {"B'"}, {"Y'"}}, {{"R'a"}, {"G'a"}, {"B'a"}, {"Y'a"}}},
    {{{"R~"}, {"G~"}, {"B~"}, {"Y~"}}, {{"R~a"}, {"G~a"}, {"B~a"}, {"Y~a"}}},
};
constexpr ComponentDescriptor component_alpha{"A"};

constexpr ModelDescriptor model(std::string_view name, ColorFamily family, Encoding encoding, Alpha alpha)
{
    const auto& colors = color_components[static_cast<int>(encoding)][alpha == Alpha::associated ? 1 : 0];
    ModelDescriptor m{name, family, encoding, alpha, 0, 0, {}};
    if (family == ColorFamily::rgb) {
        m.components = {&colors[0], &colors[1], &colors[2], &component_alpha, nullptr};
        m.color_count = 3;
    } else {
        m.components = {&colors[3], &component_alpha, nullptr, nullptr, nullptr};
        m.color_count = 1;
    }
    m.component_count = m.color_count + (alpha == Alpha::none ? 0 : 1);
    return m;
}

// CIE components (decision S2: the names, order and conventions of the
// reference catalogue). "CIE Y" is shared by XYZ, xyY and Yuv.
constexpr ComponentDescriptor cie_x{"CIE X"}, cie_y{"CIE Y"}, cie_z{"CIE Z"}, cie_small_x{"CIE x"},
    cie_small_y{"CIE y"}, cie_u{"CIE u"}, cie_v{"CIE v"}, cie_l{"CIE L"}, cie_a{"CIE a"}, cie_b{"CIE b"},
    cie_c{"CIE C(ab)"}, cie_h{"CIE H(ab)"};

constexpr ModelDescriptor cieModel(std::string_view name, ColorFamily family, const ComponentDescriptor& c0,
                                   const ComponentDescriptor& c1, const ComponentDescriptor& c2, bool alpha)
{
    return {name,
            family,
            Encoding::none,
            alpha ? Alpha::straight : Alpha::none,
            3,
            alpha ? std::size_t{4} : std::size_t{3},
            {&c0, &c1, &c2, alpha ? &component_alpha : nullptr, nullptr}};
}

constexpr ModelDescriptor cie_models[] = {
    cieModel("CIE XYZ", ColorFamily::cie_xyz, cie_x, cie_y, cie_z, false),
    cieModel("CIE XYZ alpha", ColorFamily::cie_xyz, cie_x, cie_y, cie_z, true),
    cieModel("CIE xyY", ColorFamily::cie_xyy, cie_small_x, cie_small_y, cie_y, false),
    cieModel("CIE xyY alpha", ColorFamily::cie_xyy, cie_small_x, cie_small_y, cie_y, true),
    cieModel("CIE Yuv", ColorFamily::cie_yuv, cie_y, cie_u, cie_v, false),
    cieModel("CIE Yuv alpha", ColorFamily::cie_yuv, cie_y, cie_u, cie_v, true),
    cieModel("CIE Lab", ColorFamily::cie_lab, cie_l, cie_a, cie_b, false),
    cieModel("CIE Lab alpha", ColorFamily::cie_lab, cie_l, cie_a, cie_b, true),
    cieModel("CIE LCH(ab)", ColorFamily::cie_lch, cie_l, cie_c, cie_h, false),
    cieModel("CIE LCH(ab) alpha", ColorFamily::cie_lch, cie_l, cie_c, cie_h, true),
};

// Lab encodings of ICC.1 (v4): L 0..100 and a, b -128..127 over the full code range.
constexpr TypeDescriptor cie_u8_l{"CIE u8 L", ComponentType::u8, 1, 0.0, 100.0};
constexpr TypeDescriptor cie_u8_ab{"CIE u8 ab", ComponentType::u8, 1, -128.0, 127.0};
constexpr TypeDescriptor cie_u16_l{"CIE u16 L", ComponentType::u16, 2, 0.0, 100.0};
constexpr TypeDescriptor cie_u16_ab{"CIE u16 ab", ComponentType::u16, 2, -128.0, 127.0};

// A CIE format with its own layout: (model component index, type) per stored component.
struct CieFormat {
    std::string_view name;
    const ModelDescriptor* model;
    std::size_t count;
    std::array<FormatComponent, max_components> components;
};

constexpr const ModelDescriptor& cieModelNamed(std::string_view name)
{
    for (const ModelDescriptor& m : cie_models) {
        if (m.name == name) {
            return m;
        }
    }
    return cie_models[0]; // unreachable for the names below
}

// The formats beyond "<model> float" and "<model> double".
constexpr CieFormat cie_special_formats[] = {
    {"CIE L float", &cieModelNamed("CIE Lab"), 1, {{{0, &types[4]}}}},
    {"CIE L alpha float", &cieModelNamed("CIE Lab alpha"), 2, {{{0, &types[4]}, {3, &types[4]}}}},
    {"CIE Lab u8", &cieModelNamed("CIE Lab"), 3, {{{0, &cie_u8_l}, {1, &cie_u8_ab}, {2, &cie_u8_ab}}}},
    {"CIE Lab alpha u8",
     &cieModelNamed("CIE Lab alpha"),
     4,
     {{{0, &cie_u8_l}, {1, &cie_u8_ab}, {2, &cie_u8_ab}, {3, &types[0]}}}},
    {"CIE Lab u16", &cieModelNamed("CIE Lab"), 3, {{{0, &cie_u16_l}, {1, &cie_u16_ab}, {2, &cie_u16_ab}}}},
    {"CIE Lab alpha u16",
     &cieModelNamed("CIE Lab alpha"),
     4,
     {{{0, &cie_u16_l}, {1, &cie_u16_ab}, {2, &cie_u16_ab}, {3, &types[1]}}}},
};

constexpr ModelDescriptor models[] = {
    model("RGB", ColorFamily::rgb, Encoding::linear, Alpha::none),
    model("RGBA", ColorFamily::rgb, Encoding::linear, Alpha::straight),
    model("RaGaBaA", ColorFamily::rgb, Encoding::linear, Alpha::associated),
    model("R'G'B'", ColorFamily::rgb, Encoding::space_trc, Alpha::none),
    model("R'G'B'A", ColorFamily::rgb, Encoding::space_trc, Alpha::straight),
    model("R'aG'aB'aA", ColorFamily::rgb, Encoding::space_trc, Alpha::associated),
    model("R~G~B~", ColorFamily::rgb, Encoding::perceptual, Alpha::none),
    model("R~G~B~A", ColorFamily::rgb, Encoding::perceptual, Alpha::straight),
    model("R~aG~aB~aA", ColorFamily::rgb, Encoding::perceptual, Alpha::associated),
    model("Y", ColorFamily::gray, Encoding::linear, Alpha::none),
    model("YA", ColorFamily::gray, Encoding::linear, Alpha::straight),
    model("YaA", ColorFamily::gray, Encoding::linear, Alpha::associated),
    model("Y'", ColorFamily::gray, Encoding::space_trc, Alpha::none),
    model("Y'A", ColorFamily::gray, Encoding::space_trc, Alpha::straight),
    model("Y'aA", ColorFamily::gray, Encoding::space_trc, Alpha::associated),
    model("Y~", ColorFamily::gray, Encoding::perceptual, Alpha::none),
    model("Y~A", ColorFamily::gray, Encoding::perceptual, Alpha::straight),
    model("Y~aA", ColorFamily::gray, Encoding::perceptual, Alpha::associated),
};

constexpr const char* space_names[] = {"sRGB",     "scRGB", "Rec2020", "DisplayP3", "Adobish",
                                       "ProPhoto", "Apple", "ACEScg",  "ACES2065-1"};

// Gamma TRC name: integer part, '.', millionths truncated, trailing zeros
// removed ("2.2", "2.399999" for 2.4, "3." for 3.0). trunc() instead of an int
// cast, so a gamma beyond INT_MAX (up to DBL_MAX) still gets a defined name.
std::string gammaName(double gamma)
{
    const double whole = std::trunc(gamma);
    const auto millionths = static_cast<long>((gamma - whole) * 1000000); // in [0, 1e6]
    char buffer[400]; // DBL_MAX has 309 integer digits
    std::snprintf(buffer, sizeof buffer, "%.0f.%06ld", whole, millionths);
    std::string name = buffer;
    while (!name.empty() && name.back() == '0') {
        name.pop_back();
    }
    return name;
}

// Space names are limited: explicit names keep up to 511 characters,
// generated ones 511 (chromaticities) or 510 (matrix spaces).
constexpr std::size_t max_space_name = 511;

// Generated space name; note blue before green.
std::string generatedSpaceName(std::size_t limit, double wx, double wy, double rx, double ry, double gx, double gy,
                                 double bx, double by, const std::array<const TrcDescriptor*, 3>& trc)
{
    char buffer[max_space_name + 1];
    std::snprintf(buffer, limit + 1, "space-%.4f,%.4f_%.4f,%.4f_%.4f,%.4f_%.4f,%.4f_%s,%s,%s", wx, wy, rx, ry, bx, by,
                  gx, gy, trc[0]->name.c_str(), trc[1]->name.c_str(), trc[2]->name.c_str());
    return buffer;
}

std::string spaceName(std::string_view name)
{
    return std::string(name.substr(0, max_space_name));
}

// xy of an XYZ triple. NaN when the components sum to 0, without dividing
// by zero.
std::pair<double, double> xyOf(double x, double y, double z)
{
    const double sum = x + y + z;
    if (sum == 0) {
        return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    }
    return {x / sum, y / sum};
}

Chromaticities chromaticitiesOf(const Vector3& white_xyz, const Matrix3& m)
{
    const auto xy = xyOf;
    const auto [wx, wy] = xy(white_xyz[0], white_xyz[1], white_xyz[2]);
    const auto [rx, ry] = xy(m[0][0], m[1][0], m[2][0]);
    const auto [gx, gy] = xy(m[0][1], m[1][1], m[2][1]);
    const auto [bx, by] = xy(m[0][2], m[1][2], m[2][2]);
    return {wx, wy, rx, ry, gx, gy, bx, by};
}

// Bitwise equality: -0.0 and +0.0 differ.
bool same(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool same(const Chromaticities& a, const Chromaticities& b)
{
    return same(a.white_x, b.white_x) && same(a.white_y, b.white_y) && same(a.red_x, b.red_x) &&
           same(a.red_y, b.red_y) && same(a.green_x, b.green_x) && same(a.green_y, b.green_y) &&
           same(a.blue_x, b.blue_x) && same(a.blue_y, b.blue_y);
}

bool same(const Matrix3& a, const Matrix3& b)
{
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            if (!same(a[r][k], b[r][k])) {
                return false;
            }
        }
    }
    return true;
}

bool same(const Vector3& a, const Vector3& b)
{
    return same(a[0], b[0]) && same(a[1], b[1]) && same(a[2], b[2]);
}

// Everything that can grow at run time, behind one reader/writer lock.
// Storage is std::deque: elements never move, so pointers stay valid.
struct Registry {
    std::shared_mutex mutex;
    std::deque<TrcDescriptor> trcs;
    std::deque<SpaceDescriptor> spaces;
    std::vector<std::string> base_names; // reserved up front: never reallocates
    std::vector<FormatDescriptor> base_formats;
    std::unordered_map<std::string_view, const FormatDescriptor*> base_by_name;
    std::deque<std::string> space_format_names;
    std::deque<FormatDescriptor> space_formats;
    std::map<std::pair<const FormatDescriptor*, const SpaceDescriptor*>, const FormatDescriptor*> by_space;
    const TrcDescriptor* srgb = nullptr;
    const TrcDescriptor* linear = nullptr;
    const SpaceDescriptor* srgb_space = nullptr;

    Registry();

    const TrcDescriptor& addGamma(double gamma)
    {
        for (const TrcDescriptor& t : trcs) {
            if (t.kind == TransferFunction::Kind::gamma && t.gamma == gamma) {
                return t;
            }
        }
        return trcs.emplace_back(TrcDescriptor{gammaName(gamma), TransferFunction::Kind::gamma, gamma});
    }

    const SpaceDescriptor& addSpace(std::string_view name, const Chromaticities& c,
                                     const std::array<const TrcDescriptor*, 3>& trc)
    {
        for (const SpaceDescriptor& s : spaces) {
            if (!s.from_matrix && same(s.chromaticities, c) && s.trc == trc) {
                return s; // interned by parameters; the name is ignored
            }
        }
        std::string stored_name;
        if (name.empty()) {
            stored_name = generatedSpaceName(max_space_name, c.white_x, c.white_y, c.red_x, c.red_y, c.green_x,
                                             c.green_y, c.blue_x, c.blue_y, trc);
        } else {
            stored_name = spaceName(name);
        }
        const Matrix3 to_xyz = rgbToXyzD50(c);
        return spaces.emplace_back(SpaceDescriptor{std::move(stored_name), c, xyToXyz(c.white_x, c.white_y), to_xyz,
                                                   inverse(to_xyz), trc, false});
    }

    const SpaceDescriptor& addMatrixSpace(std::string_view name, const Vector3& white, const Matrix3& m,
                                            const std::array<const TrcDescriptor*, 3>& trc)
    {
        const Chromaticities c = chromaticitiesOf(white, m);
        for (const SpaceDescriptor& s : spaces) {
            if (s.from_matrix && same(s.rgb_to_xyz, m) && same(s.white_xyz, white) && s.trc == trc) {
                return s;
            }
        }
        std::string stored_name;
        if (name.empty()) {
            stored_name = generatedSpaceName(max_space_name - 1, white[0], white[1], m[0][0], m[1][0], m[0][1],
                                             m[1][1], m[0][2], m[1][2], trc);
        } else {
            stored_name = spaceName(name);
        }
        return spaces.emplace_back(SpaceDescriptor{std::move(stored_name), c, white, m, inverse(m), trc, true});
    }

    const SpaceDescriptor* spaceNamed(std::string_view name) const
    {
        for (const SpaceDescriptor& s : spaces) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    }
};

Registry::Registry()
{
    srgb = &trcs.emplace_back(TrcDescriptor{"sRGB", TransferFunction::Kind::srgb, 0.0});
    linear = &trcs.emplace_back(TrcDescriptor{"linear", TransferFunction::Kind::linear, 0.0});
    const TrcDescriptor* gamma_2_2 = &addGamma(2.2);
    const TrcDescriptor* gamma_1_8 = &addGamma(1.8);

    const auto all = [](const TrcDescriptor* t) { return std::array<const TrcDescriptor*, 3>{t, t, t}; };
    constexpr double d65_x = 0.3127, d65_y = 0.3290;
    constexpr double aces_x = 0.32168, aces_y = 0.33767;
    // Published chromaticities (IEC 61966-2-1, ITU-R BT.2020, SMPTE EG 432-1,
    // Adobe RGB (1998), Apple RGB, ROMM/ProPhoto, ACES AP1 and AP0) with the
    // TRCs assigned to these names (decisions N4, N5).
    srgb_space = &addSpace("sRGB", srgb_chromaticities, all(srgb));
    addSpace("scRGB", srgb_chromaticities, all(linear));
    addSpace("Rec2020", {d65_x, d65_y, 0.708, 0.292, 0.170, 0.797, 0.131, 0.046}, all(srgb));
    addSpace("DisplayP3", {d65_x, d65_y, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060}, all(srgb));
    addSpace("Adobish", {d65_x, d65_y, 0.64, 0.33, 0.21, 0.71, 0.15, 0.06}, all(gamma_2_2));
    addSpace("ProPhoto", {0.34567, 0.3585, 0.7347, 0.2653, 0.1596, 0.8404, 0.0366, 0.0001}, all(gamma_1_8));
    addSpace("Apple", {d65_x, d65_y, 0.625, 0.34, 0.28, 0.595, 0.155, 0.07}, all(gamma_1_8));
    addSpace("ACEScg", {aces_x, aces_y, 0.713, 0.293, 0.165, 0.830, 0.128, 0.044}, all(linear));
    addSpace("ACES2065-1", {aces_x, aces_y, 0.7347, 0.2653, 0.0, 1.0, 0.0001, -0.0770}, all(linear));

    // Every RGB and gray model in every type; every CIE model in float and
    // double, plus the CIE formats with their own layout. All in sRGB.
    constexpr std::size_t count =
        std::size(models) * std::size(types) + std::size(cie_models) * 2 + std::size(cie_special_formats);
    base_names.reserve(count); // never reallocates: formats keep views into it
    base_formats.reserve(count);
    const auto add = [&](std::string name, const ModelDescriptor& m, const TypeDescriptor& t) {
        base_names.push_back(std::move(name));
        base_formats.push_back(uniformFormat(base_names.back(), m, t, srgb_space));
    };
    for (const ModelDescriptor& m : models) {
        for (const TypeDescriptor& t : types) {
            add(std::string(m.name) + " " + std::string(t.name), m, t);
        }
    }
    for (const ModelDescriptor& m : cie_models) {
        for (const TypeDescriptor* t : {&types[4], &types[5]}) {
            add(std::string(m.name) + " " + std::string(t->name), m, *t);
        }
    }
    for (const CieFormat& special : cie_special_formats) {
        base_names.emplace_back(special.name);
        FormatDescriptor f{base_names.back(), base_names.back(), special.model, srgb_space, special.count,
                           special.components, 0, nullptr};
        for (std::size_t i = 0; i < f.component_count; ++i) {
            f.bytes_per_pixel += f.components[i].type->size;
        }
        base_formats.push_back(f);
    }
    for (const FormatDescriptor& f : base_formats) {
        base_by_name.emplace(f.name, &f);
    }
}

Registry& registry()
{
    static Registry* instance = new Registry; // immortal: descriptors outlive every user
    return *instance;
}

const FormatDescriptor* baseFormat(std::string_view name)
{
    const Registry& r = registry();
    const auto found = r.base_by_name.find(name);
    return found != r.base_by_name.end() ? found->second : nullptr;
}

} // namespace

const FormatDescriptor& formatInSpace(const FormatDescriptor& format, const SpaceDescriptor& space)
{
    Registry& r = registry();
    const FormatDescriptor& base = *baseFormat(format.encoding);
    if (&space == r.srgb_space) {
        return base;
    }
    {
        const std::shared_lock lock(r.mutex);
        if (const auto found = r.by_space.find({&base, &space}); found != r.by_space.end()) {
            return *found->second;
        }
    }
    const std::unique_lock lock(r.mutex);
    if (const auto found = r.by_space.find({&base, &space}); found != r.by_space.end()) {
        return *found->second;
    }
    // Publish in three steps. If one throws (std::bad_alloc), the steps before it
    // are undone, so a failed attempt leaves nothing unreachable behind. The name
    // is built in place rather than moved: MSVC's Debug containers allocate
    // inside noexcept moves, where a failure would terminate the process.
    const auto slot = r.by_space.try_emplace({&base, &space}, nullptr).first;
    try {
        std::string& name = r.space_format_names.emplace_back(base.name);
        try {
            name.append("-").append(space.name);
            FormatDescriptor variant = base;
            variant.name = name;
            variant.space = &space;
            slot->second = &r.space_formats.emplace_back(variant);
        } catch (...) {
            r.space_format_names.pop_back();
            throw;
        }
    } catch (...) {
        r.by_space.erase(slot);
        throw;
    }
    return *slot->second;
}

const FormatDescriptor* findFormat(std::string_view name)
{
    if (const FormatDescriptor* f = baseFormat(name)) {
        return f;
    }
    // "<model> <type>-<space>": encodings never contain '-', space names may.
    const std::size_t dash = name.find('-');
    if (dash == std::string_view::npos) {
        return nullptr;
    }
    const FormatDescriptor* base = baseFormat(name.substr(0, dash));
    if (base == nullptr) {
        return nullptr;
    }
    const SpaceDescriptor* space = findSpace(name.substr(dash + 1));
    if (space == nullptr) {
        return nullptr;
    }
    return &formatInSpace(*base, *space);
}

const SpaceDescriptor* findSpace(std::string_view name)
{
    Registry& r = registry();
    const std::shared_lock lock(r.mutex);
    return r.spaceNamed(name);
}

const TrcDescriptor* findTrc(std::string_view name)
{
    Registry& r = registry();
    const std::shared_lock lock(r.mutex);
    for (const TrcDescriptor& t : r.trcs) {
        if (t.name == name) {
            return &t; // the first with that name
        }
    }
    return nullptr;
}

std::span<const FormatDescriptor> builtinFormats()
{
    return registry().base_formats;
}

std::span<const char* const> builtinSpaceNames() noexcept
{
    return space_names;
}

const SpaceDescriptor& srgbSpace()
{
    return *registry().srgb_space;
}

const TrcDescriptor& linearTrc()
{
    return *registry().linear;
}

const TrcDescriptor& srgbTrc()
{
    return *registry().srgb;
}

const TrcDescriptor& gammaTrc(double gamma)
{
    Registry& r = registry();
    if (std::fabs(gamma - 1.0) < 0.01) {
        return *r.linear;
    }
    const std::unique_lock lock(r.mutex);
    return r.addGamma(gamma);
}

const SpaceDescriptor& spaceFromChromaticities(std::string_view name, const Chromaticities& c,
                                                 const std::array<const TrcDescriptor*, 3>& trc)
{
    Registry& r = registry();
    const std::unique_lock lock(r.mutex);
    return r.addSpace(name, c, trc);
}

const SpaceDescriptor& spaceFromMatrix(std::string_view name, const Vector3& white_xyz, const Matrix3& rgb_to_xyz,
                                         const std::array<const TrcDescriptor*, 3>& trc)
{
    Registry& r = registry();
    const std::unique_lock lock(r.mutex);
    return r.addMatrixSpace(name, white_xyz, rgb_to_xyz, trc);
}

const SpaceDescriptor& spaceWithTrc(const SpaceDescriptor& space, const TrcDescriptor& trc)
{
    const std::array<const TrcDescriptor*, 3> all{&trc, &trc, &trc};
    if (space.trc == all) {
        return space; // already uses it on every channel 
    }
    if (space.from_matrix) {
        return spaceFromMatrix("", space.white_xyz, space.rgb_to_xyz, all); // keep the exact matrix
    }
    return spaceFromChromaticities("", space.chromaticities, all);
}

} // namespace chromaroute::detail
