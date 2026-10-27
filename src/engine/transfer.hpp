#pragma once

// Transfer functions and alpha association (docs/architecture.md, Numeric
// conventions).

#include "core/descriptors.hpp"
#include "engine/numeric.hpp"

#include <cmath>

namespace chromaroute::detail {

// Associated colors are scaled by at least this alpha.
inline constexpr double alpha_floor = 1.0 / 65536.0;

// The alpha that associated (premultiplied) colors are scaled by: alpha itself
// when |alpha| > alpha_floor, otherwise +alpha_floor (the boundary included),
// so color survives alpha 0. NaN stays NaN.
[[nodiscard]] inline double usedAlpha(double alpha) noexcept
{
    if (std::isnan(alpha)) {
        return alpha;
    }
    return std::fabs(alpha) > alpha_floor ? alpha : alpha_floor;
}

// Pure power curves map non-positive values to 0 in both directions;
// NaN propagates.
[[nodiscard]] inline double gammaPower(double value, double exponent) noexcept
{
    if (!(value > 0.0)) {
        return std::isnan(value) ? value : 0.0;
    }
    return std::pow(value, exponent);
}

[[nodiscard]] inline double toLinear(const TrcDescriptor& trc, double encoded) noexcept
{
    switch (trc.kind) {
        case TransferFunction::Kind::linear: return encoded;
        case TransferFunction::Kind::srgb: return srgbToLinear(encoded);
        case TransferFunction::Kind::gamma: return gammaPower(encoded, trc.gamma);
    }
    return encoded;
}

[[nodiscard]] inline double fromLinear(const TrcDescriptor& trc, double linear) noexcept
{
    switch (trc.kind) {
        case TransferFunction::Kind::linear: return linear;
        case TransferFunction::Kind::srgb: return linearToSrgb(linear);
        case TransferFunction::Kind::gamma: return gammaPower(linear, 1.0 / trc.gamma);
    }
    return linear;
}

} // namespace chromaroute::detail
