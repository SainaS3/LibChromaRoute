#pragma once

// Transfer functions and alpha association (docs/architecture.md, Numeric
// conventions).

#include "core/descriptors.hpp"
#include "engine/numeric.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

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

// ICC parametric curves (types 1 to 4), evaluated as ICC.1 writes them for
// every input: below the threshold the constant or linear piece, above it the
// power, whose base is taken as 0 where aX + b < 0. NaN propagates.
[[nodiscard]] inline double parametricToLinear(const ParametricCurve& p, double x) noexcept
{
    if (std::isnan(x)) {
        return x;
    }
    const auto power = [&] { return gammaPower(p.a * x + p.b, p.g); };
    switch (p.type) {
        case 1: return power();
        case 2: return power() + p.c;
        case 3: return x >= p.d ? power() : p.c * x;
        default: return x >= p.d ? power() + p.e : p.c * x + p.f;
    }
}

// The inverse: the power piece where it gives an input at or above its
// threshold, else the linear piece where it gives one below it, else the
// threshold itself (a value the curve skips, or a flat piece).
[[nodiscard]] inline double parametricFromLinear(const ParametricCurve& p, double y) noexcept
{
    if (std::isnan(y)) {
        return y;
    }
    const double offset = p.type == 2 ? p.c : p.type == 4 ? p.e : 0.0; // added to the power
    const double knee = p.type <= 2 ? -p.b / p.a : p.d;                // where the power starts
    const double v = y - offset;
    if (v > 0.0 || (v == 0.0 && p.type >= 3)) {
        const double x = (std::pow(v, 1.0 / p.g) - p.b) / p.a;
        if (x >= knee) {
            return x;
        }
    }
    if (p.type >= 3 && p.c > 0.0) {
        const double x = (y - (p.type == 4 ? p.f : 0.0)) / p.c;
        if (x < knee) {
            return x;
        }
    }
    return knee;
}

// Sampled curves: n samples at inputs i / (n - 1), linearly interpolated;
// inputs outside [0, 1] take the end samples (ICC.1 curveType).
[[nodiscard]] inline double sampledToLinear(const std::vector<double>& s, double x) noexcept
{
    if (std::isnan(x)) {
        return x;
    }
    const std::size_t last = s.size() - 1;
    if (!(x > 0.0)) {
        return s.front();
    }
    if (x >= 1.0) {
        return s.back();
    }
    const double t = x * static_cast<double>(last);
    const std::size_t i = std::min(static_cast<std::size_t>(t), last - 1);
    const double f = t - static_cast<double>(i);
    const double span = s[i + 1] - s[i];
    // Samples near the double range: their difference can overflow where the
    // value cannot, and the weighted form stays within the two samples.
    return std::isfinite(span) ? s[i] + f * span : (1.0 - f) * s[i] + f * s[i + 1];
}

// The inverse: the smallest input whose interpolated value is y. Values below
// or above every sample take the input of the first smallest or largest one.
// Samples that never decrease are searched by bisection; others in order.
[[nodiscard]] inline double sampledFromLinear(const TrcDescriptor& trc, double y) noexcept
{
    const std::vector<double>& s = trc.samples;
    if (std::isnan(y)) {
        return y;
    }
    const double last = static_cast<double>(s.size() - 1);
    const auto inSegment = [&](std::size_t i) {
        double from = s[i], to = s[i + 1], value = y;
        if (!std::isfinite(to - from) || !std::isfinite(value - from)) {
            // A difference beyond the double range: the same ratio at a
            // quarter of the size (exact, as these values are far from subnormal).
            from *= 0.25;
            to *= 0.25;
            value *= 0.25;
        }
        const double span = to - from;
        return span == 0.0 ? static_cast<double>(i) / last
                           : (static_cast<double>(i) + (value - from) / span) / last;
    };
    if (trc.monotonic) {
        if (y <= s.front()) {
            return 0.0;
        }
        if (y > s.back()) {
            // The first sample of the final plateau, the first largest.
            return static_cast<double>(std::lower_bound(s.begin(), s.end(), s.back()) - s.begin()) / last;
        }
        // The first sample at or above y; the one before it is below y.
        const auto above = std::lower_bound(s.begin() + 1, s.end(), y);
        return inSegment(static_cast<std::size_t>(above - s.begin()) - 1);
    }
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (std::fmin(s[i], s[i + 1]) <= y && y <= std::fmax(s[i], s[i + 1])) {
            return inSegment(i);
        }
    }
    const auto end = std::max_element(s.begin(), s.end()) - s.begin();
    const auto start = std::min_element(s.begin(), s.end()) - s.begin();
    return static_cast<double>(y < s[static_cast<std::size_t>(start)] ? start : end) / last;
}

[[nodiscard]] inline double toLinear(const TrcDescriptor& trc, double encoded) noexcept
{
    switch (trc.kind) {
        case TransferFunction::Kind::linear: return encoded;
        case TransferFunction::Kind::srgb: return srgbToLinear(encoded);
        case TransferFunction::Kind::gamma: return gammaPower(encoded, trc.gamma);
        case TransferFunction::Kind::parametric: return parametricToLinear(trc.curve, encoded);
        case TransferFunction::Kind::sampled: return sampledToLinear(trc.samples, encoded);
    }
    return encoded;
}

[[nodiscard]] inline double fromLinear(const TrcDescriptor& trc, double linear) noexcept
{
    switch (trc.kind) {
        case TransferFunction::Kind::linear: return linear;
        case TransferFunction::Kind::srgb: return linearToSrgb(linear);
        case TransferFunction::Kind::gamma: return gammaPower(linear, 1.0 / trc.gamma);
        case TransferFunction::Kind::parametric: return parametricFromLinear(trc.curve, linear);
        case TransferFunction::Kind::sampled: return sampledFromLinear(trc, linear);
    }
    return linear;
}

} // namespace chromaroute::detail
