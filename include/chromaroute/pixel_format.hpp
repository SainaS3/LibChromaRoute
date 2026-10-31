#pragma once

#include <chromaroute/color_space.hpp>
#include <chromaroute/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace chromaroute {

namespace detail {
struct FormatDescriptor;
} // namespace detail

// Storage type of one pixel component (type name in parentheses). Integer
// storage normally maps 0..max to 0.0..1.0; some component types map it to
// another range ("CIE u8 L": 0..255 to 0..100, "CIE u8 ab": to -128..127).
enum class ComponentType : std::uint8_t {
    u8,  // unsigned 8-bit, 0..255 maps to 0.0..1.0 ("u8")
    u16, // unsigned 16-bit, 0..65535 maps to 0.0..1.0 ("u16")
    u32, // unsigned 32-bit, 0..4294967295 maps to 0.0..1.0 ("u32")
    f16, // IEEE-754 binary16 ("half")
    f32, // IEEE-754 binary32 ("float")
    f64, // IEEE-754 binary64 ("double")
};

enum class ColorFamily : std::uint8_t {
    rgb,     // red, green, blue
    gray,    // luminance only
    cie_xyz, // CIE XYZ relative to the ICC D50 white, Y = 1 at white ("CIE XYZ")
    cie_xyy, // CIE xyY ("CIE xyY")
    cie_yuv, // CIE 1976 Y u' v' ("CIE Yuv")
    cie_lab, // CIE 1976 L*a*b*, D50 white ("CIE Lab")
    cie_lch, // CIE LCh(ab): L*, chroma, hue in degrees ("CIE LCH(ab)")
    oklab,   // Oklab: L, a, b from D65 XYZ with the CSS Color 4 matrices ("Oklab")
    ycbcr,   // Y'CbCr with BT.601 luma weights, from R~G~B~ of the space ("Y'CbCr")
    ycbcr709, // Y'CbCr with BT.709 luma weights ("Y'CbCr709")
    hsl,      // hue, saturation, lightness from R~G~B~ of the space; hue in turns ("HSL")
    hsv,      // hue, saturation, value from R~G~B~ of the space ("HSV")
    hcy,      // hue, chroma, luma from R~G~B~ with the space's luma weights ("HCY")
    cmyk,          // naive CMYK inks, 1 = full ink, from linear scRGB whatever the space ("CMYK")
    cmyk_inverted, // the same with 1 = no ink ("cmyk")
};

// How color components (never alpha) are encoded.
enum class Encoding : std::uint8_t {
    linear,     // linear light, e.g. "RGBA"
    space_trc,  // encoded with the color space's TRC, e.g. "R'G'B'A"
    perceptual, // encoded with the sRGB TRC whatever the space, e.g. "R~G~B~A";
                // Y'CbCr, HSL, HSV and HCY are computed from these values
    none,       // the model defines its values (CIE models, Oklab, CMYK)
};

enum class Alpha : std::uint8_t {
    none,
    straight,   // separate alpha, e.g. "RGBA"
    // Premultiplied: encoded color times the used alpha, e.g. "RaGaBaA",
    // "R'aG'aB'aA". The used alpha is alpha when |alpha| > 2^-16 and 2^-16
    // otherwise, so color survives alpha 0; at alpha 0 this is not ordinary
    // premultiplication (docs/architecture.md, decision N10).
    associated,
};

// An interned, immutable pixel format. Cheap to copy; equality is identity.
// Components are interleaved: colors first (R, G, B, Y, or the model's three
// components), then alpha. A few formats store only some of their model's
// components ("CIE L float" holds L of CIE Lab); the others read as 0 (alpha
// as 1).
class PixelFormat {
public:
    // Looks up a format by name: "<model> <type>" for sRGB, e.g. "R'G'B'A u8",
    // "CIE Lab alpha float" or "Y'CbCr709 double", or the same with "-<space>",
    // e.g. "RGBA float-Rec2020". A few names follow babl instead ("Oklab alpha
    // float", "Y'CbCr u8"). CIE, Oklab and CMYK values do not depend on the
    // space; Y'CbCr, HSL, HSV and HCY values are relative to it.
    static Result<PixelFormat> fromName(std::string_view name);

    // The same model and type in another space ("<model> <type>-<space>",
    // or the plain name for sRGB).
    [[nodiscard]] PixelFormat withSpace(ColorSpace space) const;

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::size_t bytesPerPixel() const noexcept;
    // Components stored per pixel.
    [[nodiscard]] std::size_t componentCount() const noexcept;
    [[nodiscard]] bool hasAlpha() const noexcept;
    // Storage type of stored component `index`. Precondition: index < componentCount().
    [[nodiscard]] ComponentType componentType(std::size_t index) const noexcept;
    [[nodiscard]] ColorFamily colorFamily() const noexcept;
    [[nodiscard]] Encoding encoding() const noexcept;
    [[nodiscard]] Alpha alpha() const noexcept;
    [[nodiscard]] ColorSpace space() const noexcept;
    // The name without the space suffix, e.g. "R'G'B'A u8" for "R'G'B'A u8-Rec2020".
    [[nodiscard]] std::string_view encodingName() const noexcept;

    friend bool operator==(PixelFormat, PixelFormat) noexcept = default;

    // Engine access; not part of the stable API. Names need not be unique
    // (two spaces may share a name), so adapters keep descriptors, not names.
    [[nodiscard]] const detail::FormatDescriptor& descriptor() const noexcept { return *descriptor_; }

private:
    // Handles come only from the factories, so they are always valid (A8).
    friend class Converter;
    explicit PixelFormat(const detail::FormatDescriptor* descriptor) noexcept : descriptor_(descriptor) {}

    const detail::FormatDescriptor* descriptor_;
};

} // namespace chromaroute
