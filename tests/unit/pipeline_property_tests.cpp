// Properties over every pair of the 147 built-in formats: converting directly equals
// converting to the destination model in double first, within the sRGB space and
// between two other spaces; every format round trips through its own model in
// double. These compare two routes through one engine; the values themselves are
// checked against the oracles elsewhere.

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

constexpr std::size_t sample_pixels = 257; // crosses two 128-pixel chunk boundaries

PixelFormat format(std::string_view name)
{
    return *PixelFormat::fromName(name);
}

PixelFormat format(std::string_view name, std::string_view space)
{
    return *PixelFormat::fromName(std::string(name) + std::string(space));
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

// Where a model's values live: value = offset + scale * u for a unit sample u.
// Alpha always uses u itself.
struct Span {
    double offset, scale;
};

Span componentSpan(chromaroute::ColorFamily family, std::size_t index)
{
    using chromaroute::ColorFamily;
    switch (family) {
        case ColorFamily::cie_lab: return index == 0 ? Span{0, 100} : Span{-128, 255};
        case ColorFamily::cie_lch: return index == 0 ? Span{0, 100} : index == 1 ? Span{0, 150} : Span{0, 360};
        case ColorFamily::cie_xyy: return index == 2 ? Span{0, 1} : Span{0, 0.8};
        case ColorFamily::cie_yuv: return index == 0 ? Span{0, 1} : Span{0, 0.6};
        case ColorFamily::oklab: return index == 0 ? Span{0, 1} : Span{-0.4, 0.8};
        case ColorFamily::ycbcr:
        case ColorFamily::ycbcr709: return index == 0 ? Span{0, 1} : Span{-0.5, 1};
        default: return Span{0, 1};
    }
}

// Deterministic inputs for any format: unit samples spanning [-0.25, 1.25] plus
// 0 and 1, mapped to each component's range (so Lab, LCH and the
// chromaticities are sampled over their own ranges, slightly beyond them), and
// converted from "<model> double" so every source type gets legal encodings.
std::vector<std::byte> sampleInput(const FormatDescriptor& f, std::string_view space = "")
{
    const std::size_t components = f.model->component_count;
    std::vector<double> values(sample_pixels * components);
    std::uint32_t state = 0x1234567u;
    for (std::size_t i = 0; i < values.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        const double u = i < components * 2 ? static_cast<double>(i % 2) : -0.25 + 1.5 * (state >> 8) / 16777216.0;
        const std::size_t index = i % components;
        const Span span = index < f.model->color_count ? componentSpan(f.model->family, index) : Span{0, 1};
        values[i] = span.offset + span.scale * u;
    }
    std::vector<std::byte> bytes(values.size() * sizeof(double));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return convert(format(doubleVariant(f), space), format(f.name, space), bytes);
}

// Direct versus staged through the destination model in double, for every pair,
// with the source in one space and the destination in another.
int stagedMismatches(std::string_view from_space, std::string_view to_space, std::FILE* log)
{
    int mismatches = 0;
    for (const FormatDescriptor& from : chromaroute::detail::builtinFormats()) {
        const PixelFormat source = format(from.name, from_space);
        const std::vector<std::byte> input = sampleInput(from, from_space);
        for (const FormatDescriptor& to : chromaroute::detail::builtinFormats()) {
            const PixelFormat destination = format(to.name, to_space);
            const PixelFormat staging = format(doubleVariant(to), to_space);
            const auto direct = convert(source, destination, input);
            const auto staged = convert(staging, destination, convert(source, staging, input));
            if (direct.empty() || direct != staged) {
                if (++mismatches <= 5) {
                    std::fprintf(log, "  mismatch: %s -> %s\n", std::string(source.name()).c_str(),
                                 std::string(destination.name()).c_str());
                }
            }
        }
    }
    return mismatches;
}

} // namespace

CR_TEST(direct_conversion_equals_conversion_through_destination_model_in_double)
{
    CR_CHECK(stagedMismatches("", "", cr_context.out()) == 0);
}

CR_TEST(direct_equals_staged_between_two_other_spaces)
{
    CR_CHECK(stagedMismatches("-Rec2020", "-ProPhoto", cr_context.out()) == 0);
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
        problems += p.name() != f.name;
        if (f.uniform_type == nullptr) {
            continue; // the CIE layouts are checked in pixel_format_tests.cpp
        }
        using chromaroute::ColorFamily;
        const ColorFamily family = p.colorFamily();
        const bool gray = family == ColorFamily::gray;
        const bool ycbcr = family == ColorFamily::ycbcr || family == ColorFamily::ycbcr709;
        const bool xyz_based = !f.model->rgbOrGray(); // CIE models and Oklab
        const bool cie_name = f.model->name.starts_with("CIE ");
        const std::string_view name = f.model->name;
        const std::size_t expected_components = (gray ? 1 : 3) + (p.hasAlpha() ? 1 : 0);
        problems += p.componentCount() != expected_components;
        problems += p.bytesPerPixel() != expected_components * f.uniform_type->size;
        problems += (p.encoding() == chromaroute::Encoding::space_trc) !=
                    (!ycbcr && name.find('\'') != std::string_view::npos);
        problems += (p.encoding() == chromaroute::Encoding::perceptual) != (ycbcr || name.find('~') != std::string_view::npos);
        problems += (p.encoding() == chromaroute::Encoding::none) != xyz_based;
        problems += p.hasAlpha() != (cie_name ? name.ends_with(" alpha") : name.find('A') != std::string_view::npos);
    }
    CR_CHECK(chromaroute::detail::builtinFormats().size() == 108 + 26 + 13);
    CR_CHECK(problems == 0);
}
