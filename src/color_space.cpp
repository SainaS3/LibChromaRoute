#include <chromaroute/color_space.hpp>

#include "core/descriptors.hpp"
#include "core/icc_profile.hpp"
#include "engine/color_math.hpp"

#include <cmath>
#include <optional>
#include <string>

namespace chromaroute {
namespace {

bool finite(const Chromaticities& c)
{
    for (double v : {c.white_x, c.white_y, c.red_x, c.red_y, c.green_x, c.green_y, c.blue_x, c.blue_y}) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

bool finite(const Matrix3& m)
{
    for (const auto& row : m) {
        for (double v : row) {
            if (!std::isfinite(v)) {
                return false;
            }
        }
    }
    return true;
}

// The derivation divides by every y, by the primaries' determinant, by the
// white's Bradford cone responses and, to invert, by the result's determinant.
// Each divisor is checked before the division (dividing by zero is undefined
// in C++), and non-finite results are rejected.
bool derivable(const Chromaticities& c)
{
    if (!finite(c) || c.white_y == 0 || c.red_y == 0 || c.green_y == 0 || c.blue_y == 0) {
        return false;
    }
    const double primaries = detail::determinant(detail::primariesMatrix(c));
    const Vector3 cone = detail::multiply(detail::bradford, detail::xyToXyz(c.white_x, c.white_y));
    if (!(std::isfinite(primaries) && primaries != 0) || cone[0] == 0 || cone[1] == 0 || cone[2] == 0) {
        return false;
    }
    const Matrix3 m = detail::rgbToXyzD50(c);
    const double det = detail::determinant(m);
    return finite(m) && std::isfinite(det) && det != 0 && finite(detail::inverse(m));
}

Error invalid(const std::string& message)
{
    return Error{ErrorCode::invalid_argument, message};
}

// Checks that create nothing, so a caller can validate everything it needs
// before it registers anything.
std::optional<Error> gammaError(double gamma)
{
    if (!std::isfinite(gamma) || !(gamma > 0)) {
        return invalid("gamma must be finite and positive");
    }
    return std::nullopt;
}

// The curve with its unused parameters cleared, or why it is invalid.
Result<ParametricCurve> checkedParametric(const ParametricCurve& curve)
{
    if (curve.type < 0 || curve.type > 4) {
        return invalid("parametric curve type must be 0 to 4");
    }
    // Only the parameters the type uses are checked and kept.
    static constexpr int used[5] = {1, 3, 4, 5, 7};
    const double given[7] = {curve.g, curve.a, curve.b, curve.c, curve.d, curve.e, curve.f};
    double kept[7] = {};
    for (int i = 0; i < used[curve.type]; ++i) {
        if (!std::isfinite(given[i])) {
            return invalid("parametric curve parameters must be finite");
        }
        kept[i] = given[i];
    }
    if (!(kept[0] > 0)) {
        return invalid("parametric curve g must be positive");
    }
    if (curve.type >= 1 && !(kept[1] > 0)) {
        return invalid("parametric curve a must be positive");
    }
    if (curve.type >= 3 && !(kept[3] >= 0)) {
        return invalid("parametric curve c must not be negative");
    }
    return ParametricCurve{curve.type, kept[0], kept[1], kept[2], kept[3], kept[4], kept[5], kept[6]};
}

std::optional<Error> sampledError(std::span<const double> samples)
{
    if (samples.size() < 2 || samples.size() > 65536) {
        return invalid("a sampled curve needs 2 to 65536 samples");
    }
    for (double s : samples) {
        if (!std::isfinite(s)) {
            return invalid("curve samples must be finite");
        }
    }
    return std::nullopt;
}

std::optional<Error> matrixSpaceError(const Vector3& white_xyz, const Matrix3& rgb_to_xyz)
{
    const double det = detail::determinant(rgb_to_xyz);
    // inverse() runs only once the determinant is known to be nonzero.
    const bool ok = std::isfinite(white_xyz[0]) && std::isfinite(white_xyz[1]) && std::isfinite(white_xyz[2]) &&
                    white_xyz[1] > 0 && finite(rgb_to_xyz) && std::isfinite(det) && det != 0 &&
                    finite(detail::inverse(rgb_to_xyz));
    if (!ok) {
        return invalid("RGB to XYZ matrix must be finite and invertible, white Y positive");
    }
    return std::nullopt;
}

} // namespace

namespace detail {

Result<const SpaceDescriptor*> makeSpace(std::string_view name, const Chromaticities& c,
                                          const std::array<const TrcDescriptor*, 3>& trc)
{
    if (!derivable(c)) {
        return invalid("chromaticities do not define an invertible RGB space");
    }
    return &spaceFromChromaticities(name, c, trc);
}

Result<const SpaceDescriptor*> makeMatrixSpace(std::string_view name, const Vector3& white_xyz,
                                                 const Matrix3& rgb_to_xyz,
                                                 const std::array<const TrcDescriptor*, 3>& trc)
{
    if (const auto error = matrixSpaceError(white_xyz, rgb_to_xyz)) {
        return *error;
    }
    return &spaceFromMatrix(name, white_xyz, rgb_to_xyz, trc);
}

} // namespace detail

TransferFunction TransferFunction::linear()
{
    return TransferFunction(&detail::linearTrc());
}

TransferFunction TransferFunction::srgb()
{
    return TransferFunction(&detail::srgbTrc());
}

Result<TransferFunction> TransferFunction::gamma(double gamma)
{
    if (const auto error = gammaError(gamma)) {
        return *error;
    }
    return TransferFunction(&detail::gammaTrc(gamma));
}

Result<TransferFunction> TransferFunction::parametric(const ParametricCurve& curve)
{
    const auto checked = checkedParametric(curve);
    if (!checked) {
        return checked.error();
    }
    if (checked->type == 0) {
        return gamma(checked->g);
    }
    return TransferFunction(&detail::parametricTrc(*checked));
}

Result<TransferFunction> TransferFunction::sampled(std::span<const double> samples)
{
    if (const auto error = sampledError(samples)) {
        return *error;
    }
    return TransferFunction(&detail::sampledTrc(samples));
}

Result<TransferFunction> TransferFunction::fromName(std::string_view name)
{
    if (const detail::TrcDescriptor* trc = detail::findTrc(name)) {
        return TransferFunction(trc);
    }
    return Error{ErrorCode::unknown_transfer_function, "unknown transfer function \"" + std::string(name) + "\""};
}

std::string_view TransferFunction::name() const noexcept
{
    return descriptor_->name;
}

TransferFunction::Kind TransferFunction::kind() const noexcept
{
    return descriptor_->kind;
}

double TransferFunction::gammaValue() const noexcept
{
    return descriptor_->gamma;
}

ParametricCurve TransferFunction::parametricCurve() const noexcept
{
    return descriptor_->curve;
}

std::span<const double> TransferFunction::samples() const noexcept
{
    return descriptor_->samples;
}

ColorSpace ColorSpace::srgb()
{
    return ColorSpace(&detail::srgbSpace());
}

Result<ColorSpace> ColorSpace::fromName(std::string_view name)
{
    if (const detail::SpaceDescriptor* space = detail::findSpace(name)) {
        return ColorSpace(space);
    }
    return Error{ErrorCode::unknown_space, "unknown color space \"" + std::string(name) + "\""};
}

Result<ColorSpace> ColorSpace::fromChromaticities(std::string_view name, const Chromaticities& chromaticities,
                                                   TransferFunction red, TransferFunction green, TransferFunction blue)
{
    const auto space =
        detail::makeSpace(name, chromaticities, {&red.descriptor(), &green.descriptor(), &blue.descriptor()});
    if (!space) {
        return space.error();
    }
    return ColorSpace(*space);
}

Result<ColorSpace> ColorSpace::fromChromaticities(std::string_view name, const Chromaticities& chromaticities,
                                                   TransferFunction trc)
{
    return fromChromaticities(name, chromaticities, trc, trc, trc);
}

Result<ColorSpace> ColorSpace::fromRgbToXyz(std::string_view name, const Vector3& white_xyz,
                                               const Matrix3& rgb_to_xyz, TransferFunction red,
                                               TransferFunction green, TransferFunction blue)
{
    const auto space = detail::makeMatrixSpace(name, white_xyz, rgb_to_xyz,
                                                 {&red.descriptor(), &green.descriptor(), &blue.descriptor()});
    if (!space) {
        return space.error();
    }
    return ColorSpace(*space);
}

namespace {

// Why a profile's curve would be rejected, without creating it.
std::optional<Error> curveError(const detail::IccCurve& curve)
{
    switch (curve.kind) {
        case detail::IccCurve::Kind::identity: return std::nullopt;
        case detail::IccCurve::Kind::gamma: return gammaError(curve.gamma);
        case detail::IccCurve::Kind::sampled: return sampledError(curve.samples);
        case detail::IccCurve::Kind::parametric: {
            const auto checked = checkedParametric(curve.parametric);
            return checked ? std::nullopt : std::optional<Error>(checked.error());
        }
    }
    return std::nullopt;
}

Result<TransferFunction> curveFrom(const detail::IccCurve& curve)
{
    switch (curve.kind) {
        case detail::IccCurve::Kind::identity: return TransferFunction::linear();
        case detail::IccCurve::Kind::gamma: return TransferFunction::gamma(curve.gamma);
        case detail::IccCurve::Kind::sampled: return TransferFunction::sampled(curve.samples);
        case detail::IccCurve::Kind::parametric: return TransferFunction::parametric(curve.parametric);
    }
    return TransferFunction::linear();
}

Error invalidProfile(const Error& error)
{
    return Error{ErrorCode::invalid_profile, "invalid ICC profile: " + error.message};
}

} // namespace

Result<ColorSpace> ColorSpace::fromIccProfile(std::string_view name, std::span<const std::byte> profile)
{
    const auto parsed = detail::readIccMatrixProfile(profile);
    if (!parsed) {
        return parsed.error();
    }
    if (parsed->lookup) { // lcms2 reads the rest, before the space is created
        const auto space = detail::deviceProfileSpace(name, profile.first(parsed->size), parsed->kind);
        if (!space) {
            return space.error();
        }
        return ColorSpace(*space);
    }
    // Everything is checked before anything is created, so a rejected profile
    // registers no curve and no space.
    const std::size_t curves = parsed->gray ? 1 : 3;
    for (std::size_t c = 0; c < curves; ++c) {
        if (const auto error = curveError(parsed->trc[c])) {
            return invalidProfile(*error); // e.g. a gamma of 0
        }
    }
    if (!parsed->gray) {
        if (const auto error = matrixSpaceError(parsed->white, parsed->rgb_to_xyz)) {
            return invalidProfile(*error); // a singular matrix or a white Y <= 0
        }
    }
    std::array<const detail::TrcDescriptor*, 3> trc{};
    for (std::size_t c = 0; c < curves; ++c) {
        trc[c] = &curveFrom(parsed->trc[c])->descriptor(); // checked above
    }
    if (parsed->gray) {
        // As in babl: sRGB's primaries with the gray curve on every channel,
        // so gray values use the curve and stay exactly neutral.
        trc[1] = trc[2] = trc[0];
        const auto space = detail::makeSpace(name, srgb().chromaticities(), trc);
        if (!space) {
            return invalidProfile(space.error());
        }
        return ColorSpace(*space);
    }
    const auto space = detail::makeMatrixSpace(name, parsed->white, parsed->rgb_to_xyz, trc);
    if (!space) {
        return invalidProfile(space.error()); // a singular matrix or a white Y <= 0
    }
    return ColorSpace(*space);
}

ColorSpace ColorSpace::withTransfer(TransferFunction trc) const
{
    return ColorSpace(&detail::spaceWithTrc(*descriptor_, trc.descriptor()));
}

bool ColorSpace::hasCmykProfile() const noexcept
{
    return descriptor_->device_profile != nullptr &&
           detail::profileKind(*descriptor_->device_profile) == detail::DeviceKind::cmyk;
}

bool ColorSpace::hasLookupTableProfile() const noexcept
{
    return descriptor_->device_profile != nullptr;
}

std::string_view ColorSpace::name() const noexcept
{
    return descriptor_->name;
}

Chromaticities ColorSpace::chromaticities() const noexcept
{
    return descriptor_->chromaticities;
}

TransferFunction ColorSpace::transfer(std::size_t channel) const noexcept
{
    return TransferFunction(descriptor_->trc[channel]);
}

const Matrix3& ColorSpace::rgbToXyz() const noexcept
{
    return descriptor_->rgb_to_xyz;
}

const Vector3& ColorSpace::luminance() const noexcept
{
    return descriptor_->luminance();
}

} // namespace chromaroute
