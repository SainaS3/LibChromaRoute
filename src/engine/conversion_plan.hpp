#pragma once

#include "core/descriptors.hpp"

#include <array>
#include <cstddef>
#include <memory>

namespace chromaroute::detail {

// Immutable recipe for converting one format to another (docs/architecture.md,
// Conversion engine). Per pixel: unpack to double; un-associate alpha; decode
// the source TRCs; gray -> RGB; RGB -> RGB matrix between spaces, or RGB ->
// luminance; encode the destination TRCs; associate alpha; pack. Steps that cancel out
// are omitted; identical formats copy bytes.
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

    using UnpackFunction = void (*)(const std::byte* in, double* out, std::size_t values);
    using PackFunction = void (*)(const double* in, std::byte* out, std::size_t values);
    using Trcs = std::array<const TrcDescriptor*, 3>; // per channel; nullptr = identity

    struct Steps {
        UnpackFunction unpack;
        PackFunction pack;
        bool unassociate;  // divide source colors by the used alpha
        std::array<bool, 3> clamp_encoded; // elided gamma decode + encode: values <= 0 become 0
        Trcs decode;
        bool apply_matrix; // source linear RGB -> destination linear RGB
        Matrix3 matrix;
        bool luminance;    // RGB -> gray with the source space's weights
        Vector3 weights;
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
