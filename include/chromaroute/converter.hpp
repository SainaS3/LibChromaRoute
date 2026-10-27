#pragma once

#include <chromaroute/pixel_format.hpp>
#include <chromaroute/result.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace chromaroute {

namespace detail {
class ConversionPlan;
} // namespace detail

// Converts pixels from one format to another. Immutable after creation: one
// Converter may be used from any number of threads at once, and copies share
// the same underlying plan.
class Converter {
public:
    // Unsupported pairs return an error. Running out of memory throws
    // std::bad_alloc, as standard containers do; nothing is left half-built,
    // so a later call can succeed.
    static Result<Converter> create(PixelFormat source, PixelFormat destination);

    // Copies share the plan. There are no move operations, so "moving" copies:
    // a Converter never loses its plan, and a moved-from one stays usable.
    Converter(const Converter&) = default;
    Converter& operator=(const Converter&) = default;

    // Converts pixel_count pixels. Both spans must hold at least
    // pixel_count * bytesPerPixel() bytes of their format and must not
    // overlap. pixel_count == 0 succeeds without touching either buffer.
    // Buffers need no particular alignment.
    Status process(std::span<const std::byte> source, std::span<std::byte> destination,
                   std::size_t pixel_count) const;

    [[nodiscard]] PixelFormat sourceFormat() const noexcept;
    [[nodiscard]] PixelFormat destinationFormat() const noexcept;

private:
    explicit Converter(std::shared_ptr<const detail::ConversionPlan> plan) noexcept;

    std::shared_ptr<const detail::ConversionPlan> plan_;
};

} // namespace chromaroute
