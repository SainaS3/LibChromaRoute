#pragma once

#include "core/descriptors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace chromaroute::detail {

// Immutable recipe for converting one format to another (docs/architecture.md,
// Conversion engine). Per pixel: unpack each stored component to double in
// model order (missing ones read as 0, alpha as 1); bring the colors to the
// hub: linear RGB of the source space (un-associate alpha, decode the TRCs,
// gray -> RGB), or for CIE models XYZ (normalized to the D50 white when a Lab
// or LCH side is involved, L*a*b* between Lab and LCH); one 3x3 matrix or a
// luminance between hubs; then the reverse on the destination side (encode,
// associate) and pack. Steps that cancel out are omitted; identical formats
// copy bytes.
class ConversionPlan {
public:
    // Shared, cached plan for the pair; nullptr when the pair is unsupported.
    static std::shared_ptr<const ConversionPlan> get(const FormatDescriptor& source,
                                                     const FormatDescriptor& destination);

    // Preconditions (validated by Converter): both buffers hold pixel_count
    // pixels and do not overlap.
    void run(const std::byte* source, std::byte* destination, std::size_t pixel_count) const noexcept;

    [[nodiscard]] const FormatDescriptor& source() const noexcept { return *source_; }
    [[nodiscard]] const FormatDescriptor& destination() const noexcept { return *destination_; }

    // `count` values; bytes are `byte_stride` apart, doubles `value_stride` apart.
    using UnpackFunction = void (*)(const TypeDescriptor& type, const std::byte* in, std::size_t byte_stride,
                                    double* out, std::size_t value_stride, std::size_t count);
    using PackFunction = void (*)(const TypeDescriptor& type, const double* in, std::size_t value_stride,
                                  std::byte* out, std::size_t byte_stride, std::size_t count);
    using Trcs = std::array<const TrcDescriptor*, 3>; // per channel; nullptr = identity

    // What the colors are between the source and destination models.
    enum class Hub : std::uint8_t {
        rgb,        // linear RGB (source space before the matrix, destination space after)
        xyz,        // CIE XYZ, when no side is Lab or LCH
        normalized, // XYZ normalized to the D50 white, when a side is Lab or LCH
        lab,        // CIE L*a*b* (between Lab and LCH, no XYZ round trip)
        same,       // the same CIE model on both sides: values are copied
    };

    struct Steps {
        std::array<UnpackFunction, max_components> unpack; // per stored source component
        std::array<PackFunction, max_components> pack;     // per stored destination component
        std::size_t fill_count;                             // model components the source does not store
        std::array<std::size_t, max_components> fill_index;
        Hub hub;
        bool unassociate;  // divide source colors by the used alpha
        std::array<bool, 3> clamp_encoded; // elided gamma decode + encode: values <= 0 become 0
        Trcs decode;
        bool apply_matrix; // between hubs: RGB -> RGB, RGB -> (normalized) XYZ or back
        Matrix3 matrix;
        bool luminance;    // RGB -> gray with the source space's weights
        Vector3 weights;
        bool take_y;       // XYZ -> gray: Y itself
        bool neutral_to_cie; // RGB or gray -> CIE: neutral pixels skip the matrix (N7, N8)
        bool binary32_hue; // LCH stored as binary32: the hue rules hold for the stored values
        Trcs encode;
        bool associate;    // multiply destination colors by the used alpha
        bool nonfinite_alpha_to_nan; // kept association: colors x (used alpha / used alpha)
    };

    ConversionPlan(const FormatDescriptor& source, const FormatDescriptor& destination, const Steps& steps) noexcept;

private:
    void convertModels(const double* in, double* out, std::size_t pixels) const noexcept;

    const FormatDescriptor* source_;
    const FormatDescriptor* destination_;
    bool copy_;
    Steps steps_;
};

} // namespace chromaroute::detail
