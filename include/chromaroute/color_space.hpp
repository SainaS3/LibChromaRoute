#pragma once

#include <chromaroute/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace chromaroute {

namespace detail {
struct TrcDescriptor;
struct SpaceDescriptor;
} // namespace detail

// An ICC parametric curve (ICC.1 parametricCurveType), decoding X to linear Y:
//   0: Y = X^g
//   1: Y = (aX + b)^g for X >= -b/a, else 0
//   2: Y = (aX + b)^g + c for X >= -b/a, else c
//   3: Y = (aX + b)^g for X >= d, else cX
//   4: Y = (aX + b)^g + e for X >= d, else cX + f
// Parameters a type does not use are ignored.
struct ParametricCurve {
    int type;
    double g, a, b, c, d, e, f;
};

// A transfer function (TRC): how encoded color values relate to linear light.
// Interned and immutable; equality is identity.
class TransferFunction {
public:
    enum class Kind : std::uint8_t {
        linear,
        srgb,       // IEC 61966-2-1
        gamma,      // pure power: decode x^gamma, encode x^(1/gamma); x <= 0 maps to 0
        parametric, // ICC parametric curve types 1 to 4 (type 0 is a gamma)
        sampled,    // samples at evenly spaced inputs over [0, 1], linearly interpolated
    };

    // The first call of the process builds the descriptor registry, so these
    // can throw std::bad_alloc; they cannot fail otherwise.
    [[nodiscard]] static TransferFunction linear();
    [[nodiscard]] static TransferFunction srgb();
    // Gammas within 0.01 of 1 give linear(). gamma must be finite and > 0.
    static Result<TransferFunction> gamma(double gamma);
    // An ICC parametric curve: type 0 to 4, its parameters finite, g > 0, and
    // for types 1 to 4 a > 0, for types 3 and 4 c >= 0. Type 0 gives gamma(g).
    static Result<TransferFunction> parametric(const ParametricCurve& curve);
    // A sampled curve: 2 to 65536 finite samples of the linear value at inputs
    // 0, 1/(n-1), ..., 1. Inputs outside [0, 1] take the end samples.
    static Result<TransferFunction> sampled(std::span<const double> samples);
    // "sRGB", "linear", or the name of a curve created earlier ("2.2", "1.8", ...).
    static Result<TransferFunction> fromName(std::string_view name);

    // Name: gamma curves are named from their value with millionths truncated
    // and trailing zeros removed ("2.2", "2.399999", "3."); parametric curves
    // "para<type>:<parameters>", sampled ones "table<n>:<hash of the samples>".
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] double gammaValue() const noexcept; // the exponent for Kind::gamma, otherwise 0
    // Kind::parametric: the curve, unused parameters 0; otherwise all 0.
    [[nodiscard]] ParametricCurve parametricCurve() const noexcept;
    // Kind::sampled: the samples; otherwise empty.
    [[nodiscard]] std::span<const double> samples() const noexcept;

    friend bool operator==(TransferFunction, TransferFunction) noexcept = default;

    // Engine access; not part of the stable API.
    [[nodiscard]] const detail::TrcDescriptor& descriptor() const noexcept { return *descriptor_; }

private:
    friend class ColorSpace;
    explicit TransferFunction(const detail::TrcDescriptor* descriptor) noexcept : descriptor_(descriptor) {}

    const detail::TrcDescriptor* descriptor_;
};

// CIE 1931 xy chromaticities of the white point and the three primaries.
struct Chromaticities {
    double white_x, white_y;
    double red_x, red_y;
    double green_x, green_y;
    double blue_x, blue_y;
};

using Vector3 = std::array<double, 3>;
using Matrix3 = std::array<Vector3, 3>; // row-major

// An RGB color space: primaries and white point (stored as a linear RGB -> CIE
// XYZ matrix adapted to the ICC D50 white with Bradford) plus one TRC per
// channel. Interned and immutable; equality is identity.
class ColorSpace {
public:
    // IEC 61966-2-1 sRGB (decision N4). Like TransferFunction::srgb(), the
    // first call can throw std::bad_alloc.
    [[nodiscard]] static ColorSpace srgb();

    // Built-in names: sRGB, scRGB, Rec2020, DisplayP3, Adobish, Apple, ProPhoto,
    // ACEScg, ACES2065-1; plus names given to spaces created below.
    static Result<ColorSpace> fromName(std::string_view name);

    // Creates (or finds) a space from chromaticities. Spaces are interned by
    // their parameters: identical chromaticities and TRCs return the existing
    // space whatever name is passed. An empty name generates a
    // "space-<w>_<r>_<b>_<g>_<trcs>" name.
    static Result<ColorSpace> fromChromaticities(std::string_view name, const Chromaticities& chromaticities,
                                                  TransferFunction red, TransferFunction green,
                                                  TransferFunction blue);
    static Result<ColorSpace> fromChromaticities(std::string_view name, const Chromaticities& chromaticities,
                                                  TransferFunction trc);

    // Creates (or finds) a space from a D50-adapted RGB -> XYZ matrix and the
    // XYZ of its white point, as stored in matrix/TRC ICC profiles.
    static Result<ColorSpace> fromRgbToXyz(std::string_view name, const Vector3& white_xyz,
                                              const Matrix3& rgb_to_xyz, TransferFunction red,
                                              TransferFunction green, TransferFunction blue);
    // Creates (or finds) a space from an ICC profile (version 2 or 4) with an
    // XYZ connection space. An RGB matrix/TRC profile gives fromRgbToXyz() with
    // its colorants (rXYZ, gXYZ, bXYZ) as the matrix, its white point (wtpt)
    // and its tone curves (rTRC, gTRC, bTRC); a gray profile gives sRGB's
    // primaries with its gray curve (kTRC) on every channel. Lookup tables in a
    // profile that also has a matrix are ignored. Errors: invalid_profile for
    // malformed bytes, unsupported_profile for other profiles (lookup tables
    // only, CMYK, Lab connection space). Checking stops at the first problem,
    // so an unsupported profile is not necessarily valid otherwise.
    static Result<ColorSpace> fromIccProfile(std::string_view name, std::span<const std::byte> profile);

    // The same primaries with another TRC on every channel.
    // Both inputs are valid handles, so this cannot fail (it may throw
    // std::bad_alloc, like any creation).
    [[nodiscard]] ColorSpace withTransfer(TransferFunction trc) const;

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] Chromaticities chromaticities() const noexcept;
    // Precondition: channel < 3.
    [[nodiscard]] TransferFunction transfer(std::size_t channel) const noexcept;
    [[nodiscard]] const Matrix3& rgbToXyz() const noexcept;
    // Y row of rgbToXyz(): weights of linear R, G, B in luminance.
    [[nodiscard]] const Vector3& luminance() const noexcept;

    friend bool operator==(ColorSpace, ColorSpace) noexcept = default;

    // Engine access; not part of the stable API.
    [[nodiscard]] const detail::SpaceDescriptor& descriptor() const noexcept { return *descriptor_; }

private:
    // Handles come only from the factories, so they are always valid (A8).
    friend class PixelFormat;
    explicit ColorSpace(const detail::SpaceDescriptor* descriptor) noexcept : descriptor_(descriptor) {}

    const detail::SpaceDescriptor* descriptor_;
};

} // namespace chromaroute
