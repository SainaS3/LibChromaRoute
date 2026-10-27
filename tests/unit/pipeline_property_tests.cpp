// Properties over all 48 x 48 registered pairs: converting directly equals
// converting to the destination model in double first; every format round
// trips through its own model in double.

#include "support/test_harness.hpp"

#include "core/descriptors.hpp"

#include <chromaroute/chromaroute.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;
using chromaroute::detail::FormatDescriptor;

namespace {

constexpr std::size_t sample_pixels = 97; // not a multiple of the chunk size

PixelFormat format(std::string_view name)
{
    return *PixelFormat::fromName(name);
}

std::string doubleVariant(const FormatDescriptor& f)
{
    return std::string(f.model->name) + " double";
}

std::vector<std::byte> convert(const PixelFormat& from, const PixelFormat& to, const std::vector<std::byte>& input)
{
    const Converter converter = *Converter::create(from, to);
    const std::size_t pixels = input.size() / from.bytesPerPixel();
    std::vector<std::byte> output(pixels * to.bytesPerPixel());
    if (!converter.process(input, output, pixels)) {
        output.clear();
    }
    return output;
}

// Deterministic inputs for any format: values derived from doubles spanning
// [-0.25, 1.25] plus 0, 1 and exact half-ULP-ish points, converted from
// "<model> double" so every source type is exercised with legal encodings.
std::vector<std::byte> sampleInput(const FormatDescriptor& f)
{
    const std::size_t components = f.model->component_count;
    std::vector<double> values(sample_pixels * components);
    std::uint32_t state = 0x1234567u;
    for (std::size_t i = 0; i < values.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        values[i] = i < components * 2 ? static_cast<double>(i % 2) : -0.25 + 1.5 * (state >> 8) / 16777216.0;
    }
    std::vector<std::byte> bytes(values.size() * sizeof(double));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return convert(format(doubleVariant(f)), format(f.name), bytes);
}

} // namespace

CR_TEST(direct_conversion_equals_conversion_through_destination_model_in_double)
{
    int mismatches = 0;
    for (const FormatDescriptor& from : chromaroute::detail::builtinFormats()) {
        const std::vector<std::byte> input = sampleInput(from);
        for (const FormatDescriptor& to : chromaroute::detail::builtinFormats()) {
            const auto direct = convert(format(from.name), format(to.name), input);
            const auto staged =
                convert(format(doubleVariant(to)), format(to.name), convert(format(from.name), format(doubleVariant(to)), input));
            if (direct.empty() || direct != staged) {
                if (++mismatches <= 5) {
                    std::fprintf(cr_context.out(), "  mismatch: %s -> %s\n", std::string(from.name).c_str(),
                                 std::string(to.name).c_str());
                }
            }
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(every_format_round_trips_through_its_model_in_double)
{
    int mismatches = 0;
    for (const FormatDescriptor& f : chromaroute::detail::builtinFormats()) {
        const std::vector<std::byte> input = sampleInput(f);
        const auto back = convert(format(doubleVariant(f)), format(f.name), convert(format(f.name), format(doubleVariant(f)), input));
        if (back != input && ++mismatches <= 5) {
            std::fprintf(cr_context.out(), "  round trip changed %s\n", std::string(f.name).c_str());
        }
    }
    CR_CHECK(mismatches == 0);
}

CR_TEST(all_formats_have_consistent_metadata)
{
    int problems = 0;
    for (const FormatDescriptor& f : chromaroute::detail::builtinFormats()) {
        const PixelFormat p = format(f.name);
        const bool gray = p.colorFamily() == chromaroute::ColorFamily::gray;
        const std::size_t expected_components = (gray ? 1 : 3) + (p.hasAlpha() ? 1 : 0);
        problems += p.componentCount() != expected_components;
        problems += p.bytesPerPixel() != expected_components * f.type->size;
        problems += p.name() != f.name;
        problems += (p.encoding() == chromaroute::Encoding::space_trc) != (f.model->name.find('\'') != std::string_view::npos);
        problems += p.hasAlpha() != (f.model->name.back() == 'A');
    }
    CR_CHECK(chromaroute::detail::builtinFormats().size() == 108);
    CR_CHECK(problems == 0);
}
