// Exhaustive R1 encode check: every binary32 value in [0, 1] goes through
// "RGBA float" -> "R'G'B'A u8" and must match the exact result implied by the
// oracle thresholds, for the color channels (sRGB OETF) and for alpha.
// About 1.07e9 values per channel kind; host Release only (docs/testing.md).

#include "data/srgb_oracle_data.h"

#include <chromaroute/chromaroute.hpp>

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace {

constexpr std::uint32_t last_bits = 0x3F800000u; // 1.0f; all non-negative floats up to it, in order
constexpr std::size_t batch_pixels = 1u << 20;

// Counts thresholds at or below the given bit pattern; values only increase,
// so the position only moves forward.
struct ThresholdWalker {
    const std::uint32_t* thresholds;
    int passed = 0;
    int expected(std::uint32_t bits)
    {
        while (passed < 255 && thresholds[passed] <= bits) {
            ++passed;
        }
        return passed;
    }
};

// lane 0..2: the value goes into that color channel; lane 3: into alpha.
long long run(const Converter& encode, bool color)
{
    ThresholdWalker walker{color ? chromaroute_oracle_linear_f32_to_srgb_u8_threshold
                                 : chromaroute_oracle_f32_to_u8_threshold};
    const std::size_t per_pixel = color ? 3 : 1;
    std::vector<float> rgba(batch_pixels * 4);
    std::vector<std::uint8_t> out(batch_pixels * 4);
    std::vector<std::uint8_t> expected(batch_pixels * 3);
    long long mismatches = 0;
    std::uint64_t next = 0;
    while (next <= last_bits) {
        std::size_t pixels = 0;
        std::size_t values = 0;
        for (; pixels < batch_pixels && next <= last_bits; ++pixels) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                rgba[pixels * 4 + lane] = 0.0f;
            }
            for (std::size_t k = 0; k < per_pixel && next <= last_bits; ++k, ++next) {
                const auto bits = static_cast<std::uint32_t>(next);
                rgba[pixels * 4 + (color ? k : 3)] = std::bit_cast<float>(bits);
                expected[values++] = static_cast<std::uint8_t>(walker.expected(bits));
            }
        }
        if (!encode.process(std::as_bytes(std::span(rgba)), std::as_writable_bytes(std::span(out)), pixels)) {
            std::printf("conversion failed\n");
            return -1;
        }
        for (std::size_t v = 0; v < values; ++v) {
            const std::size_t lane = color ? v % 3 : 3;
            const std::uint8_t got = out[(v / per_pixel) * 4 + lane];
            if (got != expected[v] && ++mismatches <= 10) {
                std::printf("  mismatch: %s input %a -> %d, expected %d\n", color ? "color" : "alpha",
                            static_cast<double>(rgba[(v / per_pixel) * 4 + lane]), got, expected[v]);
            }
        }
    }
    return mismatches;
}

} // namespace

int main()
{
    const Converter encode =
        *Converter::create(*PixelFormat::fromName("RGBA float"), *PixelFormat::fromName("R'G'B'A u8"));
    const auto start = std::chrono::steady_clock::now();
    const long long color = run(encode, true);
    const long long alpha = run(encode, false);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("exhaustive R1 encode over %u values per kind: color mismatches %lld, alpha mismatches %lld (%.1f s)\n",
                last_bits + 1, color, alpha, seconds);
    return color == 0 && alpha == 0 ? 0 : 1;
}
