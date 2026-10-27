#pragma once

// Interned, immutable descriptors behind the public handle types. They live
// until the process ends, so pointers to them never dangle. Every name is
// NUL-terminated (the C layer hands them out as C strings).

#include <chromaroute/color_space.hpp>
#include <chromaroute/pixel_format.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace chromaroute::detail {

struct TypeDescriptor {
    std::string_view name; // type name, e.g. "u16", "half"
    ComponentType type;
    std::size_t size;      // bytes per component
};

struct ComponentDescriptor {
    std::string_view name; // component name, e.g. "R'a", "Y", "A"
};

struct ModelDescriptor {
    std::string_view name; // model name, e.g. "R'aG'aB'aA", "Y~"
    ColorFamily family;
    Encoding encoding;
    Alpha alpha;
    std::size_t component_count;                   // colors, then alpha
    std::array<const ComponentDescriptor*, 4> components;

    [[nodiscard]] std::size_t colorCount() const noexcept { return family == ColorFamily::rgb ? 3 : 1; }
    [[nodiscard]] bool hasAlpha() const noexcept { return alpha != Alpha::none; }
    [[nodiscard]] bool associated() const noexcept { return alpha == Alpha::associated; }
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

struct FormatDescriptor {
    std::string_view name;     // "<model> <type>" in sRGB, "<model> <type>-<space>" otherwise
    std::string_view encoding; // "<model> <type>"
    const ModelDescriptor* model;
    const TypeDescriptor* type; // every component shares one type
    const SpaceDescriptor* space;

    [[nodiscard]] std::size_t bytesPerPixel() const noexcept { return model->component_count * type->size; }
};

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
