// Exhaustive CIE checks, host Release only (docs/testing.md):
//  1. All 16,777,216 "R'G'B' u8" colors (sRGB and ProPhoto) go to "CIE Lab u8"
//     directly, and the result must equal the staged route "R'G'B' u8" ->
//     "CIE Lab double" -> "CIE Lab u8". Every code must also be the defined
//     rounding of its double value (docs/architecture.md, integer range types),
//     decided here by exact comparisons rather than the library's arithmetic.
//     This covers the per-component range packing for every input; the values
//     themselves are checked against the oracle in unit/cie_tests.cpp.
//  2. The same colors to "CIE LCH(ab) float": every stored hue is in [0, 360),
//     and a stored chroma of 0 has hue 0.
//  3. Every binary32 input of the four range encodings (CIE u8 L, u8 ab, u16 L,
//     u16 ab) gets the exact round-half-up code, decided in 64-bit integers.

#include "engine/numeric.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <thread>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace {

// The defined code of a double value: s = (v - lo) * 255 in double, then round
// half up of s / (hi - lo), clamped, NaN -> 0. Decided by bisection on exact
// comparisons: 2 s and (2k - 1)(hi - lo) are exact in double.
int definedCode(double v, double lo, double hi)
{
    if (!(v > lo)) {
        return 0;
    }
    if (v >= hi) {
        return 255;
    }
    const double twice_scaled = 2.0 * ((v - lo) * 255.0);
    int code = 0;
    int high = 255;
    while (code < high) {
        const int mid = (code + high + 1) / 2;
        if (twice_scaled >= (2.0 * mid - 1.0) * (hi - lo)) {
            code = mid;
        } else {
            high = mid - 1;
        }
    }
    return code;
}

int labColors(const char* source_name)
{
    const PixelFormat source = *PixelFormat::fromName(source_name);
    const Converter direct = *Converter::create(source, *PixelFormat::fromName("CIE Lab u8"));
    const Converter to_double = *Converter::create(source, *PixelFormat::fromName("CIE Lab double"));
    const Converter from_double =
        *Converter::create(*PixelFormat::fromName("CIE Lab double"), *PixelFormat::fromName("CIE Lab u8"));
    const Converter to_lch = *Converter::create(source, *PixelFormat::fromName("CIE LCH(ab) float"));

    constexpr std::size_t batch = 1u << 16; // one value of the first channel per batch
    std::vector<std::uint8_t> rgb(batch * 3), lab8(batch * 3), staged8(batch * 3);
    std::vector<double> lab(batch * 3);
    std::vector<float> lch(batch * 3);
    long mismatches = 0, inexact = 0, bad_hue = 0;
    for (std::size_t r = 0; r < 256; ++r) {
        for (std::size_t i = 0; i < batch; ++i) {
            rgb[i * 3] = static_cast<std::uint8_t>(r);
            rgb[i * 3 + 1] = static_cast<std::uint8_t>(i >> 8);
            rgb[i * 3 + 2] = static_cast<std::uint8_t>(i & 0xFF);
        }
        const auto in = std::as_bytes(std::span(rgb));
        if (!direct.process(in, std::as_writable_bytes(std::span(lab8)), batch) ||
            !to_double.process(in, std::as_writable_bytes(std::span(lab)), batch) ||
            !from_double.process(std::as_bytes(std::span(lab)), std::as_writable_bytes(std::span(staged8)), batch) ||
            !to_lch.process(in, std::as_writable_bytes(std::span(lch)), batch)) {
            std::printf("conversion failed\n");
            return 1;
        }
        for (std::size_t k = 0; k < batch * 3; ++k) {
            const double lo = k % 3 == 0 ? 0.0 : -128.0;
            const double hi = k % 3 == 0 ? 100.0 : 127.0;
            mismatches += lab8[k] != staged8[k];
            inexact += lab8[k] != definedCode(lab[k], lo, hi);
        }
        for (std::size_t p = 0; p < batch; ++p) {
            const float chroma = lch[p * 3 + 1];
            const float hue = lch[p * 3 + 2];
            bad_hue += !(hue >= 0.0f && hue < 360.0f) || std::signbit(hue) || (chroma == 0.0f && hue != 0.0f);
        }
    }
    std::printf("%s: 16777216 colors to CIE Lab u8: %ld differ from the staged route, %ld codes not the defined "
                "rounding; to CIE LCH(ab) float: %ld hues outside the rules\n",
                source_name, mismatches, inexact, bad_hue);
    return mismatches == 0 && inexact == 0 && bad_hue == 0 ? 0 : 1;
}

// Every binary32 pattern through encodeRange<UInt>(x, lo, hi), against round
// half up of q = (x - lo) max / (hi - lo): code c is right when
// (2c - 1)(hi - lo) <= 2 (x - lo) max < (2c + 1)(hi - lo). For |x| >= 2^-12,
// x * 2^36 is an integer and every product stays below 2^61. Smaller |x| lie
// within max * 2^-12 / (hi - lo) <= 0.17 of the integer q at x = 0 (0, 128 or
// 32896), so their code is that integer.
template <typename UInt>
long rangeMismatches(std::int64_t lo, std::int64_t hi, std::uint64_t first, std::uint64_t last)
{
    constexpr std::int64_t max = std::numeric_limits<UInt>::max();
    constexpr std::int64_t one = std::int64_t{1} << 36;
    const std::int64_t range = hi - lo;
    const std::int64_t code_at_zero = -lo * max / range; // exact: 0, 128 or 32896
    long wrong = 0;
    for (std::uint64_t pattern = first; pattern < last; ++pattern) {
        const auto bits = static_cast<std::uint32_t>(pattern);
        const float x = std::bit_cast<float>(bits);
        const std::int64_t code = chromaroute::detail::encodeRange<UInt>(x, static_cast<double>(lo), static_cast<double>(hi));
        if (!(x > static_cast<float>(lo))) {
            wrong += code != 0; // NaN and values at or below lo
            continue;
        }
        if (x >= static_cast<float>(hi)) {
            wrong += code != max;
            continue;
        }
        const std::uint32_t biased_exponent = (bits >> 23) & 0xFF;
        if (biased_exponent < 115) { // |x| < 2^-12
            wrong += code != code_at_zero;
            continue;
        }
        const std::int64_t magnitude = ((std::int64_t{1} << 23) | (bits & 0x7FFFFF)) << (biased_exponent - 114);
        const std::int64_t x_scaled = bits >> 31 ? -magnitude : magnitude; // x * 2^36
        const std::int64_t twice_scaled = 2 * (x_scaled - lo * one) * max;
        wrong += !((2 * code - 1) * range * one <= twice_scaled && twice_scaled < (2 * code + 1) * range * one);
    }
    return wrong;
}

int rangeEncodings()
{
    constexpr std::uint64_t patterns = std::uint64_t{1} << 32;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<long> wrong[4] = {};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            const std::uint64_t first = patterns * t / threads;
            const std::uint64_t last = patterns * (t + 1) / threads;
            wrong[0] += rangeMismatches<std::uint8_t>(0, 100, first, last);
            wrong[1] += rangeMismatches<std::uint8_t>(-128, 127, first, last);
            wrong[2] += rangeMismatches<std::uint16_t>(0, 100, first, last);
            wrong[3] += rangeMismatches<std::uint16_t>(-128, 127, first, last);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("every binary32 input: %ld (CIE u8 L), %ld (CIE u8 ab), %ld (CIE u16 L), %ld (CIE u16 ab) codes not "
                "the exact rounding\n",
                wrong[0].load(), wrong[1].load(), wrong[2].load(), wrong[3].load());
    return wrong[0] == 0 && wrong[1] == 0 && wrong[2] == 0 && wrong[3] == 0 ? 0 : 1;
}

} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    int failed = labColors("R'G'B' u8");
    failed += labColors("R'G'B' u8-ProPhoto");
    failed += rangeEncodings();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("exhaustive CIE: %s (%.1f s)\n", failed == 0 ? "PASS" : "FAIL", seconds);
    return failed == 0 ? 0 : 1;
}
