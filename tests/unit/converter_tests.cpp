// Converter contract: creation, buffer validation, zero length, overflow,
// overlap, alignment, chunking, identity copies.

#include "support/test_harness.hpp"

#include "core/descriptors.hpp"
#include "engine/conversion_plan.hpp"

#include <chromaroute/chromaroute.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using chromaroute::Converter;
using chromaroute::ErrorCode;
using chromaroute::PixelFormat;

namespace {

PixelFormat u8Format()
{
    return *PixelFormat::fromName("R'G'B'A u8");
}

PixelFormat floatFormat()
{
    return *PixelFormat::fromName("RGBA float");
}

std::vector<std::byte> u8Pattern(std::size_t pixels)
{
    std::vector<std::byte> bytes(pixels * 4);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte(static_cast<unsigned char>((i * 37 + 11) & 0xFF));
    }
    return bytes;
}

} // namespace

CR_TEST(every_registered_pair_is_supported)
{
    int failures = 0;
    for (const auto& from : chromaroute::detail::builtinFormats()) {
        for (const auto& to : chromaroute::detail::builtinFormats()) {
            const auto converter = Converter::create(*PixelFormat::fromName(from.name), *PixelFormat::fromName(to.name));
            if (!converter || converter->sourceFormat().name() != from.name ||
                converter->destinationFormat().name() != to.name) {
                ++failures;
            }
        }
    }
    CR_CHECK(failures == 0);
}

CR_TEST(every_builtin_space_pair_is_supported)
{
    int failures = 0;
    for (const char* a : chromaroute::detail::builtinSpaceNames()) {
        for (const char* b : chromaroute::detail::builtinSpaceNames()) {
            const PixelFormat from = u8Format().withSpace(*chromaroute::ColorSpace::fromName(a));
            const PixelFormat to = floatFormat().withSpace(*chromaroute::ColorSpace::fromName(b));
            failures += !Converter::create(from, to).has_value();
        }
    }
    CR_CHECK(failures == 0);
}

CR_TEST(plan_cache_returns_one_shared_plan_per_pair)
{
    using namespace chromaroute::detail;
    const FormatDescriptor& u8 = *findFormat("R'G'B'A u8");
    const FormatDescriptor& f32 = *findFormat("RGBA float");
    CR_CHECK(ConversionPlan::get(u8, f32) != nullptr);
    CR_CHECK(ConversionPlan::get(u8, f32) == ConversionPlan::get(u8, f32));
    CR_CHECK(ConversionPlan::get(u8, f32) != ConversionPlan::get(f32, u8));
}

CR_TEST(moved_from_converter_stays_usable)
{
    Converter a = *Converter::create(*PixelFormat::fromName("R'G'B'A u8"), *PixelFormat::fromName("RGBA float"));
    const Converter b = std::move(a); // copies: a Converter never loses its plan
    Converter c = *Converter::create(*PixelFormat::fromName("RGBA u8"), *PixelFormat::fromName("RGBA u16"));
    c = std::move(a);
    const std::uint8_t in[4] = {255, 0, 128, 255};
    float out_a[4] = {}, out_b[4] = {}, out_c[4] = {};
    CR_CHECK(a.sourceFormat() == *PixelFormat::fromName("R'G'B'A u8"));
    CR_CHECK(a.process(std::as_bytes(std::span(in)), std::as_writable_bytes(std::span(out_a)), 1));
    CR_CHECK(b.process(std::as_bytes(std::span(in)), std::as_writable_bytes(std::span(out_b)), 1));
    CR_CHECK(c.process(std::as_bytes(std::span(in)), std::as_writable_bytes(std::span(out_c)), 1));
    CR_CHECK(out_a[0] == 1.0f && std::memcmp(out_a, out_b, sizeof out_a) == 0 && std::memcmp(out_a, out_c, sizeof out_a) == 0);
}

CR_TEST(zero_pixels_succeeds_without_buffers)
{
    const auto converter = *Converter::create(u8Format(), floatFormat());
    CR_CHECK(converter.process({}, {}, 0).has_value());
}

CR_TEST(undersized_buffers_are_rejected_untouched)
{
    const auto converter = *Converter::create(u8Format(), floatFormat());
    std::vector<std::byte> source = u8Pattern(4);
    std::vector<std::byte> destination(4 * 16, std::byte{0xAB});

    const auto short_source = converter.process(std::span(source).first(15), destination, 4);
    CR_CHECK(!short_source.has_value());
    CR_CHECK(short_source.error().code == ErrorCode::buffer_too_small);

    const auto short_destination = converter.process(source, std::span(destination).first(63), 4);
    CR_CHECK(!short_destination.has_value());
    CR_CHECK(short_destination.error().code == ErrorCode::buffer_too_small);

    bool untouched = true;
    for (std::byte b : destination) {
        untouched = untouched && b == std::byte{0xAB};
    }
    CR_CHECK(untouched);
    CR_CHECK(converter.process(source, destination, 4).has_value());
}

CR_TEST(pixel_count_overflow_is_rejected)
{
    const auto converter = *Converter::create(u8Format(), floatFormat());
    std::byte dummy[16]{};
    const std::size_t huge = std::numeric_limits<std::size_t>::max() / 16 + 1;
    const auto status = converter.process(std::span<const std::byte>(dummy, 16), std::span<std::byte>(dummy, 16), huge);
    CR_CHECK(!status.has_value());
    CR_CHECK(status.error().code == ErrorCode::size_overflow);
}

CR_TEST(overlapping_buffers_are_rejected)
{
    const auto identity = *Converter::create(floatFormat(), floatFormat());
    std::vector<std::byte> buffer(64 * 16);
    const std::span<std::byte> all(buffer);

    const auto in_place = identity.process(all.first(32 * 16), all.first(32 * 16), 32);
    CR_CHECK(!in_place.has_value());
    CR_CHECK(in_place.error().code == ErrorCode::overlapping_buffers);

    const auto partial = identity.process(all.first(32 * 16), all.subspan(31 * 16, 32 * 16), 32);
    CR_CHECK(!partial.has_value());

    const auto adjacent = identity.process(all.first(32 * 16), all.subspan(32 * 16, 32 * 16), 32);
    CR_CHECK(adjacent.has_value());
}

CR_TEST(identity_copies_bytes_exactly)
{
    const auto identity = *Converter::create(floatFormat(), floatFormat());
    const std::uint32_t patterns[] = {0x7FC00001u, 0xFFC12345u, 0x7F800000u, 0x80000000u, 0x00000001u, 0x3F800000u,
                                      0xFF7FFFFFu, 0x12345678u}; // NaN payloads, ±Inf, -0, subnormal, …
    std::byte source[sizeof patterns];
    std::memcpy(source, patterns, sizeof patterns);
    std::byte destination[sizeof patterns]{};
    CR_CHECK(identity.process(source, destination, 2).has_value());
    CR_CHECK(std::memcmp(source, destination, sizeof patterns) == 0);

    const auto u8_identity = *Converter::create(u8Format(), u8Format());
    const std::vector<std::byte> bytes = u8Pattern(300);
    std::vector<std::byte> copy(bytes.size());
    CR_CHECK(u8_identity.process(bytes, copy, 300).has_value());
    CR_CHECK(copy == bytes);
}

CR_TEST(unaligned_buffers_and_chunk_boundaries_match_single_pixels)
{
    const auto decode = *Converter::create(u8Format(), floatFormat());
    for (std::size_t count : {1u, 127u, 128u, 129u, 257u, 1000u}) {
        const std::vector<std::byte> pixels = u8Pattern(count);
        // Offset both buffers by one byte so float accesses are misaligned.
        std::vector<std::byte> source(pixels.size() + 1);
        std::memcpy(source.data() + 1, pixels.data(), pixels.size());
        std::vector<std::byte> bulk(count * 16 + 1);
        const auto source_span = std::span<const std::byte>(source).subspan(1);
        CR_CHECK(decode.process(source_span, std::span(bulk).subspan(1), count).has_value());

        bool same = true;
        for (std::size_t i = 0; i < count; ++i) {
            std::byte single[16];
            CR_CHECK(decode.process(source_span.subspan(i * 4, 4), single, 1).has_value());
            same = same && std::memcmp(single, bulk.data() + 1 + i * 16, 16) == 0;
        }
        CR_CHECK(same);
    }
}
