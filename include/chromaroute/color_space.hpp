#pragma once

#include <chromaroute/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace chromaroute {

namespace detail {
struct TrcDescriptor;
struct SpaceDescriptor;
} // namespace detail

// A transfer function (TRC): how encoded color values relate to linear light.
// Interned and immutable; equality is identity.
class TransferFunction {
public:
    enum class Kind : std::uint8_t {
        linear,
        srgb,  // IEC 61966-2-1
        gamma, // pure power: decode x^gamma, encode x^(1/gamma); x <= 0 maps to 0
    };

    // The first call of the process builds the descriptor registry, so these
    // can throw std::bad_alloc; they cannot fail otherwise.
    [[nodiscard]] static TransferFunction linear();
    [[nodiscard]] static TransferFunction srgb();
    // Gammas within 0.01 of 1 give linear(). gamma must be finite and > 0.
    static Result<TransferFunction> gamma(double gamma);
    // "sRGB", "linear", or the name of a gamma curve created earlier ("2.2", "1.8", ...).
    static Result<TransferFunction> fromName(std::string_view name);

    // Name: gamma curves are named from their value with
    // millionths truncated and trailing zeros removed ("2.2", "2.399999", "3.").
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] double gammaValue() const noexcept; // the exponent for Kind::gamma, otherwise 0

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
    explicit ColorSpace(const detail::SpaceDescriptor* descriptor) noexcept : descriptor_(descriptor) {}

private:
    const detail::SpaceDescriptor* descriptor_;
};

} // namespace chromaroute
