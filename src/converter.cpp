#include <chromaroute/converter.hpp>

#include "engine/conversion_plan.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace chromaroute {
namespace {

bool overlaps(const std::byte* a, std::size_t a_size, const std::byte* b, std::size_t b_size) noexcept
{
    const auto a_begin = reinterpret_cast<std::uintptr_t>(a);
    const auto b_begin = reinterpret_cast<std::uintptr_t>(b);
    return a_begin < b_begin + b_size && b_begin < a_begin + a_size;
}

std::string byteCountMessage(const char* which, std::size_t have, std::size_t need)
{
    return std::string(which) + " buffer holds " + std::to_string(have) + " bytes; " + std::to_string(need) +
           " needed";
}

} // namespace

Converter::Converter(std::shared_ptr<const detail::ConversionPlan> plan) noexcept : plan_(std::move(plan)) {}

Result<Converter> Converter::create(PixelFormat source, PixelFormat destination)
{
    std::shared_ptr<const detail::ConversionPlan> plan =
        detail::ConversionPlan::get(source.descriptor(), destination.descriptor());
    if (!plan) {
        return Error{ErrorCode::unsupported_conversion, "no conversion from \"" + std::string(source.name()) +
                                                            "\" to \"" + std::string(destination.name()) + "\""};
    }
    return Converter(std::move(plan));
}

Status Converter::process(std::span<const std::byte> source, std::span<std::byte> destination,
                          std::size_t pixel_count) const
{
    if (pixel_count == 0) {
        return {};
    }
    const std::size_t source_stride = plan_->source().bytesPerPixel();
    const std::size_t destination_stride = plan_->destination().bytesPerPixel();
    constexpr std::size_t max_size = std::numeric_limits<std::size_t>::max();
    if (pixel_count > max_size / source_stride || pixel_count > max_size / destination_stride) {
        return Error{ErrorCode::size_overflow, "pixel count " + std::to_string(pixel_count) + " overflows size_t"};
    }
    const std::size_t source_bytes = pixel_count * source_stride;
    const std::size_t destination_bytes = pixel_count * destination_stride;
    if (source.size() < source_bytes) {
        return Error{ErrorCode::buffer_too_small, byteCountMessage("source", source.size(), source_bytes)};
    }
    if (destination.size() < destination_bytes) {
        return Error{ErrorCode::buffer_too_small,
                     byteCountMessage("destination", destination.size(), destination_bytes)};
    }
    if (overlaps(source.data(), source_bytes, destination.data(), destination_bytes)) {
        return Error{ErrorCode::overlapping_buffers, "source and destination buffers overlap"};
    }
    plan_->run(source.data(), destination.data(), pixel_count);
    return {};
}

PixelFormat Converter::sourceFormat() const noexcept
{
    return PixelFormat(&plan_->source());
}

PixelFormat Converter::destinationFormat() const noexcept
{
    return PixelFormat(&plan_->destination());
}

} // namespace chromaroute
