#include "support/test_harness.hpp"

#include "core/descriptors.hpp"

#include <chromaroute/chromaroute.hpp>

#include <string>

using chromaroute::Alpha;
using chromaroute::ComponentType;
using chromaroute::Encoding;
using chromaroute::ErrorCode;
using chromaroute::PixelFormat;

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
    for (const char* name : {"", "RGBA", "rgba float", "R'G'B'A u8 ", "RGBA u15", "R u8", "CIE Lab float", "RGBA float-", "RGBA float-NoSuchSpace", "-sRGB"}) {
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
    for (const auto& format : chromaroute::detail::builtinFormats()) {
        const std::string expected = std::string(format.model->name) + " " + std::string(format.type->name);
        CR_CHECK(format.name == expected);
        CR_CHECK(format.space->name == "sRGB");
    }
}
