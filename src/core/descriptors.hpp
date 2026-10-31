#pragma once

// Interned, immutable descriptors behind the public handle types. They live
// until the process ends, so pointers to them never dangle. Every name is
// NUL-terminated (the C layer hands them out as C strings).

#include <chromaroute/color_space.hpp>
#include <chromaroute/pixel_format.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace chromaroute::detail {

// Most components a model or format has.
inline constexpr std::size_t max_components = 5;

struct TypeDescriptor {
    std::string_view name; // type name, e.g. "u16", "half", "CIE u8 L", "u8-luma"
    ComponentType type;    // storage
    std::size_t size;      // bytes per component
    // Integer storage maps codes code_min..code_max to min..max (decision A7);
    // code_max 0 stands for the largest code of the storage. Codes outside
    // code_min..code_max decode to min or max (decision S8). Float storage
    // ignores these.
    double min = 0.0;
    double max = 1.0;
    std::uint32_t code_min = 0;
    std::uint32_t code_max = 0;

    [[nodiscard]] bool ranged() const noexcept
    {
        return min != 0.0 || max != 1.0 || code_min != 0 || code_max != 0;
    }
};

struct ComponentDescriptor {
    std::string_view name; // component name, e.g. "R'a", "Y", "A", "CIE L"
};

struct ModelDescriptor {
    std::string_view name; // model name, e.g. "R'aG'aB'aA", "Y~", "CIE Lab alpha"
    ColorFamily family;
    Encoding encoding;
    Alpha alpha;
    std::size_t color_count;     // colors come first, then alpha
    std::size_t component_count; // colors plus alpha
    std::array<const ComponentDescriptor*, max_components> components;

    [[nodiscard]] std::size_t colorCount() const noexcept { return color_count; }
    [[nodiscard]] bool hasAlpha() const noexcept { return alpha != Alpha::none; }
    [[nodiscard]] bool associated() const noexcept { return alpha == Alpha::associated; }
    // RGB, Y'CbCr, HSL, HSV and HCY: three colors in a format's space. The
    // others are computed from R~G~B~ of that space (decision S4), so the
    // engine treats them as RGB with one more step on the encoded values.
    [[nodiscard]] bool rgbLike() const noexcept { return family == ColorFamily::rgb || fromTildeRgb(); }
    [[nodiscard]] bool fromTildeRgb() const noexcept
    {
        return family == ColorFamily::ycbcr || family == ColorFamily::ycbcr709 || family == ColorFamily::hsl ||
               family == ColorFamily::hsv || family == ColorFamily::hcy;
    }
    // Everything else (CIE models, Oklab) goes through XYZ.
    [[nodiscard]] bool rgbOrGray() const noexcept { return rgbLike() || family == ColorFamily::gray; }
};

struct TrcDescriptor {
    std::string name; // name: "sRGB", "linear", "2.2", ...
    TransferFunction::Kind kind;
    double gamma;     // exponent for Kind::gamma, otherwise 0
};

struct SpaceDescriptor {
    std::string name;
    Chromaticities chromaticities; // as given, or derived from the matrix columns
    Vector3 white_xyz;             // as given for matrix spaces, else from the white's xy
    Matrix3 rgb_to_xyz;            // linear RGB -> XYZ (D50)
    Matrix3 xyz_to_rgb;
    std::array<const TrcDescriptor*, 3> trc;
    bool from_matrix;              // created by fromRgbToXyz

    [[nodiscard]] const Vector3& luminance() const noexcept { return rgb_to_xyz[1]; }
};

// One stored component of a format: which model component it holds, and how.
struct FormatComponent {
    std::size_t model_index;
    const TypeDescriptor* type;
};

struct FormatDescriptor {
    std::string_view name;     // "<model> <type>" in sRGB, "<model> <type>-<space>" otherwise
    std::string_view encoding; // the name without the space suffix
    const ModelDescriptor* model;
    const SpaceDescriptor* space;
    // Stored components, in memory order. A format may store a subset of its
    // model's components ("CIE L float"); the missing ones read as 0, alpha as 1.
    std::size_t component_count;
    std::array<FormatComponent, max_components> components;
    std::size_t bytes_per_pixel;
    // Set when the format stores every model component in model order with
    // one type (the common case): then the whole pixel shares `type`.
    const TypeDescriptor* uniform_type;

    [[nodiscard]] std::size_t bytesPerPixel() const noexcept { return bytes_per_pixel; }
    [[nodiscard]] bool storesAlpha() const noexcept
    {
        for (std::size_t i = 0; i < component_count; ++i) {
            if (model->hasAlpha() && components[i].model_index == model->color_count) {
                return true;
            }
        }
        return false;
    }
};

// A format storing every model component in order, all of one type.
[[nodiscard]] constexpr FormatDescriptor uniformFormat(std::string_view name, const ModelDescriptor& model,
                                                       const TypeDescriptor& type,
                                                       const SpaceDescriptor* space) noexcept
{
    FormatDescriptor f{name, name, &model, space, model.component_count, {}, model.component_count * type.size, &type};
    for (std::size_t i = 0; i < model.component_count; ++i) {
        f.components[i] = {i, &type};
    }
    return f;
}

// ---- lookups: exact, case-sensitive; nullptr when unknown; thread-safe ----

// "<model> <type>" or "<model> <type>-<space>" (creates the space variant on demand).
[[nodiscard]] const FormatDescriptor* findFormat(std::string_view name);
// The variant of an sRGB format in another space (the format itself for sRGB).
[[nodiscard]] const FormatDescriptor& formatInSpace(const FormatDescriptor& format, const SpaceDescriptor& space);
[[nodiscard]] const SpaceDescriptor* findSpace(std::string_view name);
[[nodiscard]] const TrcDescriptor* findTrc(std::string_view name);

// These four may be the first registry access, which allocates: not noexcept.
[[nodiscard]] std::span<const FormatDescriptor> builtinFormats(); // the sRGB formats
[[nodiscard]] std::span<const char* const> builtinSpaceNames() noexcept;
[[nodiscard]] const SpaceDescriptor& srgbSpace();
[[nodiscard]] const TrcDescriptor& linearTrc();
[[nodiscard]] const TrcDescriptor& srgbTrc();

// ---- validated creation (src/color_space.cpp); errors are invalid_argument ----

[[nodiscard]] Result<const SpaceDescriptor*> makeSpace(std::string_view name, const Chromaticities& c,
                                                        const std::array<const TrcDescriptor*, 3>& trc);
[[nodiscard]] Result<const SpaceDescriptor*> makeMatrixSpace(std::string_view name, const Vector3& white_xyz,
                                                               const Matrix3& rgb_to_xyz,
                                                               const std::array<const TrcDescriptor*, 3>& trc);

// ---- creation: interned by parameters; inputs validated by the callers ----

// Precondition: finite gamma > 0. Gammas within 0.01 of 1 return linearTrc().
[[nodiscard]] const TrcDescriptor& gammaTrc(double gamma);
// Precondition: chromaticities give an invertible primaries matrix.
[[nodiscard]] const SpaceDescriptor& spaceFromChromaticities(std::string_view name, const Chromaticities& c,
                                                               const std::array<const TrcDescriptor*, 3>& trc);
// Precondition: finite, invertible matrix; white_xyz with positive Y.
[[nodiscard]] const SpaceDescriptor& spaceFromMatrix(std::string_view name, const Vector3& white_xyz,
                                                       const Matrix3& rgb_to_xyz,
                                                       const std::array<const TrcDescriptor*, 3>& trc);
[[nodiscard]] const SpaceDescriptor& spaceWithTrc(const SpaceDescriptor& space, const TrcDescriptor& trc);

} // namespace chromaroute::detail
