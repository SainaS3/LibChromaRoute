#include <chromaroute/pixel_format.hpp>

#include "core/descriptors.hpp"

#include <string>

namespace chromaroute {

Result<PixelFormat> PixelFormat::fromName(std::string_view name)
{
    if (const detail::FormatDescriptor* descriptor = detail::findFormat(name)) {
        return PixelFormat(descriptor);
    }
    return Error{ErrorCode::unknown_format, "unknown pixel format \"" + std::string(name) + "\""};
}

std::string_view PixelFormat::name() const noexcept
{
    return descriptor_->name;
}

std::size_t PixelFormat::bytesPerPixel() const noexcept
{
    return descriptor_->bytesPerPixel();
}

std::size_t PixelFormat::componentCount() const noexcept
{
    return descriptor_->component_count;
}

bool PixelFormat::hasAlpha() const noexcept
{
    return descriptor_->storesAlpha();
}

ComponentType PixelFormat::componentType(std::size_t index) const noexcept
{
    return descriptor_->components[index].type->type;
}

ColorFamily PixelFormat::colorFamily() const noexcept
{
    return descriptor_->model->family;
}

Encoding PixelFormat::encoding() const noexcept
{
    return descriptor_->model->encoding;
}

Alpha PixelFormat::alpha() const noexcept
{
    return descriptor_->model->alpha;
}

PixelFormat PixelFormat::withSpace(ColorSpace space) const
{
    return PixelFormat(&detail::formatInSpace(*descriptor_, space.descriptor()));
}

ColorSpace PixelFormat::space() const noexcept
{
    return ColorSpace(descriptor_->space);
}

std::string_view PixelFormat::encodingName() const noexcept
{
    return descriptor_->encoding;
}

} // namespace chromaroute
