#include <chromaroute/color_space.hpp>

#include "core/descriptors.hpp"
#include "engine/color_math.hpp"

#include <cmath>
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
    const double det = determinant(rgb_to_xyz);
    // inverse() runs only once the determinant is known to be nonzero.
    const bool ok = std::isfinite(white_xyz[0]) && std::isfinite(white_xyz[1]) && std::isfinite(white_xyz[2]) &&
                    white_xyz[1] > 0 && finite(rgb_to_xyz) && std::isfinite(det) && det != 0 &&
                    finite(inverse(rgb_to_xyz));
    if (!ok) {
        return invalid("RGB to XYZ matrix must be finite and invertible, white Y positive");
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
    if (!std::isfinite(gamma) || !(gamma > 0)) {
        return invalid("gamma must be finite and positive");
    }
    return TransferFunction(&detail::gammaTrc(gamma));
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

ColorSpace ColorSpace::withTransfer(TransferFunction trc) const
{
    return ColorSpace(&detail::spaceWithTrc(*descriptor_, trc.descriptor()));
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
