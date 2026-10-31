#include "support/test_harness.hpp"

#include "core/descriptors.hpp"

#include <chromaroute/chromaroute.hpp>

#include <cstddef>
#include <string>
#include <type_traits>

using chromaroute::Alpha;
using chromaroute::ComponentType;
using chromaroute::Encoding;
using chromaroute::ErrorCode;
using chromaroute::PixelFormat;

// Handles come only from the library's factories (A8): no public constructor
// takes a descriptor pointer or nullptr, and copying stays available.
static_assert(!std::is_constructible_v<PixelFormat, const chromaroute::detail::FormatDescriptor*>);
static_assert(!std::is_constructible_v<PixelFormat, std::nullptr_t>);
static_assert(!std::is_constructible_v<chromaroute::ColorSpace, const chromaroute::detail::SpaceDescriptor*>);
static_assert(!std::is_constructible_v<chromaroute::ColorSpace, std::nullptr_t>);
static_assert(!std::is_constructible_v<chromaroute::TransferFunction, const chromaroute::detail::TrcDescriptor*>);
static_assert(std::is_nothrow_copy_constructible_v<PixelFormat> && std::is_nothrow_copy_assignable_v<PixelFormat>);
static_assert(std::is_nothrow_copy_constructible_v<chromaroute::ColorSpace> &&
              std::is_nothrow_copy_assignable_v<chromaroute::ColorSpace>);

CR_TEST(nonlinear_rgba_u8_metadata)
{
    const auto format = PixelFormat::fromName("R'G'B'A u8");
    CR_CHECK(format.has_value());
    CR_CHECK(format->name() == "R'G'B'A u8");
    CR_CHECK(format->bytesPerPixel() == 4);
    CR_CHECK(format->componentCount() == 4);
    CR_CHECK(format->hasAlpha());
    CR_CHECK(format->alpha() == Alpha::straight);
    CR_CHECK(format->encoding() == Encoding::space_trc);
    for (std::size_t i = 0; i < 4; ++i) {
        CR_CHECK(format->componentType(i) == ComponentType::u8);
    }
}

CR_TEST(linear_rgba_float_metadata)
{
    const auto format = PixelFormat::fromName("RGBA float");
    CR_CHECK(format.has_value());
    CR_CHECK(format->name() == "RGBA float");
    CR_CHECK(format->bytesPerPixel() == 16);
    CR_CHECK(format->componentCount() == 4);
    CR_CHECK(format->hasAlpha());
    CR_CHECK(format->encoding() == Encoding::linear);
    CR_CHECK(format->componentType(3) == ComponentType::f32);
}

CR_TEST(lookups_are_interned)
{
    CR_CHECK(*PixelFormat::fromName("RGBA float") == *PixelFormat::fromName("RGBA float"));
    CR_CHECK(!(*PixelFormat::fromName("RGBA float") == *PixelFormat::fromName("R'G'B'A u8")));
}

CR_TEST(unknown_names_are_errors)
{
    for (const char* name : {"", "RGBA", "rgba float", "R'G'B'A u8 ", "RGBA u15", "R u8", "CIE Lab half", "CIE XYZ u8",
                             "CIE L u8", "CIE Lab", "RGBA float-", "RGBA float-NoSuchSpace", "-sRGB", "OklabA float",
                             "Oklab alpha double", "Oklab u8", "Y'CbCr half", "Y'CbCrA u8", "Y'CbCr709 u8",
                             "Y'CbCr u8-luma"}) {
        const auto format = PixelFormat::fromName(name);
        CR_CHECK(!format.has_value());
        if (!format.has_value()) {
            CR_CHECK(format.error().code == ErrorCode::unknown_format);
            CR_CHECK(format.error().message.find(name) != std::string::npos);
        }
    }
}

CR_TEST(builtin_names_follow_model_type_grammar)
{
    // Formats of one type are "<model> <type>", except babl's "Oklab alpha float"
    // (OklabA in float); the formats with their own layout keep their catalogue
    // names.
    int layouts = 0;
    for (const auto& format : chromaroute::detail::builtinFormats()) {
        if (format.uniform_type != nullptr) {
            const std::string expected = std::string(format.model->name) + " " + std::string(format.uniform_type->name);
            CR_CHECK(format.name == expected || (format.name == "Oklab alpha float" && expected == "OklabA float"));
        } else {
            ++layouts;
        }
        CR_CHECK(format.space->name == "sRGB");
    }
    CR_CHECK(layouts == 7);
    CR_CHECK(chromaroute::detail::builtinFormats().size() == 108 + 26 + 13);
}

CR_TEST(cie_formats_have_their_catalogue_layouts)
{
    using chromaroute::ColorFamily;
    struct Expected {
        const char* name;
        ColorFamily family;
        std::size_t components, bytes;
        bool alpha;
        ComponentType first, last;
    };
    const Expected cases[] = {
        {"CIE XYZ float", ColorFamily::cie_xyz, 3, 12, false, ComponentType::f32, ComponentType::f32},
        {"CIE XYZ alpha float", ColorFamily::cie_xyz, 4, 16, true, ComponentType::f32, ComponentType::f32},
        {"CIE xyY double", ColorFamily::cie_xyy, 3, 24, false, ComponentType::f64, ComponentType::f64},
        {"CIE Yuv alpha double", ColorFamily::cie_yuv, 4, 32, true, ComponentType::f64, ComponentType::f64},
        {"CIE LCH(ab) alpha float", ColorFamily::cie_lch, 4, 16, true, ComponentType::f32, ComponentType::f32},
        {"CIE L float", ColorFamily::cie_lab, 1, 4, false, ComponentType::f32, ComponentType::f32},
        {"CIE L alpha float", ColorFamily::cie_lab, 2, 8, true, ComponentType::f32, ComponentType::f32},
        {"CIE Lab u8", ColorFamily::cie_lab, 3, 3, false, ComponentType::u8, ComponentType::u8},
        {"CIE Lab alpha u8", ColorFamily::cie_lab, 4, 4, true, ComponentType::u8, ComponentType::u8},
        {"CIE Lab u16", ColorFamily::cie_lab, 3, 6, false, ComponentType::u16, ComponentType::u16},
        {"CIE Lab alpha u16", ColorFamily::cie_lab, 4, 8, true, ComponentType::u16, ComponentType::u16},
    };
    for (const Expected& e : cases) {
        const auto f = PixelFormat::fromName(e.name);
        CR_CHECK(f.has_value());
        if (!f) {
            continue;
        }
        CR_CHECK(f->colorFamily() == e.family && f->encoding() == Encoding::none);
        CR_CHECK(f->componentCount() == e.components && f->bytesPerPixel() == e.bytes);
        CR_CHECK(f->hasAlpha() == e.alpha && f->alpha() == (e.alpha ? Alpha::straight : Alpha::none));
        CR_CHECK(f->componentType(0) == e.first && f->componentType(e.components - 1) == e.last);
    }
    // The Lab u8 components map their codes to L and a/b ranges; alpha is plain u8.
    const auto& lab8 = PixelFormat::fromName("CIE Lab alpha u8")->descriptor();
    CR_CHECK(lab8.components[0].type->name == "CIE u8 L" && lab8.components[1].type->name == "CIE u8 ab");
    CR_CHECK(lab8.components[2].type->name == "CIE u8 ab" && lab8.components[3].type->name == "u8");
    const auto& l_alpha = PixelFormat::fromName("CIE L alpha float")->descriptor();
    CR_CHECK(l_alpha.components[0].model_index == 0 && l_alpha.components[1].model_index == 3);
    // Formats with a space exist for CIE models too, named as usual.
    const auto in_rec = PixelFormat::fromName("CIE Lab u8-Rec2020");
    CR_CHECK(in_rec.has_value() && in_rec->space().name() == "Rec2020" && in_rec->bytesPerPixel() == 3);
    // In another space every format keeps its layout, component for component.
    for (const Expected& e : cases) {
        const auto base = PixelFormat::fromName(e.name);
        const auto other = PixelFormat::fromName(std::string(e.name) + "-ProPhoto");
        CR_CHECK(base.has_value() && other.has_value());
        if (!base || !other) {
            continue;
        }
        const auto& b = base->descriptor();
        const auto& o = other->descriptor();
        bool same = o.component_count == b.component_count && o.bytes_per_pixel == b.bytes_per_pixel &&
                    o.uniform_type == b.uniform_type && o.model == b.model;
        for (std::size_t i = 0; i < b.component_count; ++i) {
            same = same && o.components[i].model_index == b.components[i].model_index &&
                   o.components[i].type == b.components[i].type;
        }
        CR_CHECK(same);
    }
}
