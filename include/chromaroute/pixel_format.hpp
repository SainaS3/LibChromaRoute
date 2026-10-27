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

// Storage type of one pixel component (type name in parentheses).
enum class ComponentType : std::uint8_t {
    u8,  // unsigned 8-bit, 0..255 maps to 0.0..1.0 ("u8")
    u16, // unsigned 16-bit, 0..65535 maps to 0.0..1.0 ("u16")
    u32, // unsigned 32-bit, 0..4294967295 maps to 0.0..1.0 ("u32")
    f16, // IEEE-754 binary16 ("half")
    f32, // IEEE-754 binary32 ("float")
    f64, // IEEE-754 binary64 ("double")
};

enum class ColorFamily : std::uint8_t {
    rgb,  // red, green, blue
    gray, // luminance only
};

// How color components (never alpha) are encoded.
enum class Encoding : std::uint8_t {
    linear,     // linear light, e.g. "RGBA"
    space_trc,  // encoded with the color space's TRC, e.g. "R'G'B'A"
    perceptual, // encoded with the sRGB TRC whatever the space, e.g. "R~G~B~A"
};

enum class Alpha : std::uint8_t {
    none,
    straight,   // separate alpha, e.g. "RGBA"
    associated, // premultiplied: encoded color times alpha, e.g. "RaGaBaA", "R'aG'aB'aA"
};

// An interned, immutable pixel format. Cheap to copy; equality is identity.
// Components are interleaved: colors first (R, G, B or Y), then alpha.
class PixelFormat {
public:
    // Looks up a format by name: "<model> <type>" for sRGB,
    // e.g. "R'G'B'A u8", or "<model> <type>-<space>", e.g. "RGBA float-Rec2020".
    static Result<PixelFormat> fromName(std::string_view name);

    // The same model and type in another space ("<model> <type>-<space>",
    // or the plain name for sRGB).
    [[nodiscard]] PixelFormat withSpace(ColorSpace space) const;

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::size_t bytesPerPixel() const noexcept;
    [[nodiscard]] std::size_t componentCount() const noexcept;
    [[nodiscard]] bool hasAlpha() const noexcept;
    // Precondition: index < componentCount().
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
    explicit PixelFormat(const detail::FormatDescriptor* descriptor) noexcept : descriptor_(descriptor) {}

private:
    const detail::FormatDescriptor* descriptor_;
};

} // namespace chromaroute
