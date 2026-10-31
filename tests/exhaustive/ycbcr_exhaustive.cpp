// Exhaustive Y'CbCr u8 checks, host Release only (docs/testing.md):
//  1. All 16,777,216 "R'G'B' u8" colors in sRGB to "Y'CbCr u8". There R'G'B' is
//     already the sRGB-curve encoding, so each code is c_lo plus round half up
//     of an exact rational: Y' = 16 + 219 (299 R + 587 G + 114 B) / 255000, and
//     Cb, Cr likewise. Decided in integers. A value exactly halfway between two
//     codes is a tie that double evaluation may round either way; such cases are
//     counted, not compared. Every other value lies at least 1 / (2 * 451860)
//     from a tie, far beyond double's error.
//  2. The same colors in ProPhoto (decode the 1.8 TRC, then the sRGB curve, S4):
//     direct must equal the staged route through "Y'CbCr double", and every code
//     must be the defined rounding of its double value.
//  3. Every binary32 input of the u8-luma and u8-chroma encoders against the
//     exact round half up, decided in 64-bit integers.

#include "engine/numeric.hpp"

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace {

// floor(n / d + 1/2) for d > 0; *tie is set when n / d is exactly halfway.
long roundHalfUp(std::int64_t n, std::int64_t d, bool* tie)
{
    const std::int64_t twice = 2 * n + d; // 2 (n / d + 1/2) d
    *tie = twice % (2 * d) == 0;
    std::int64_t q = twice / (2 * d);
    if (twice % (2 * d) < 0) {
        --q; // floor for negative numerators
    }
    return static_cast<long>(q);
}

int srgbColors()
{
    const Converter direct = *Converter::create(*PixelFormat::fromName("R'G'B' u8"), *PixelFormat::fromName("Y'CbCr u8"));
    constexpr std::size_t batch = 1u << 16;
    std::vector<std::uint8_t> rgb(batch * 3), ycc(batch * 3);
    long wrong = 0, ties = 0;
    for (std::size_t r = 0; r < 256; ++r) {
        for (std::size_t i = 0; i < batch; ++i) {
            rgb[i * 3] = static_cast<std::uint8_t>(r);
            rgb[i * 3 + 1] = static_cast<std::uint8_t>(i >> 8);
            rgb[i * 3 + 2] = static_cast<std::uint8_t>(i & 0xFF);
        }
        if (!direct.process(std::as_bytes(std::span(rgb)), std::as_writable_bytes(std::span(ycc)), batch)) {
            std::printf("conversion failed\n");
            return 1;
        }
        for (std::size_t i = 0; i < batch; ++i) {
            const std::int64_t R = rgb[i * 3], G = rgb[i * 3 + 1], B = rgb[i * 3 + 2];
            const std::int64_t luma = 299 * R + 587 * G + 114 * B; // 1000 * 255 * Y'
            bool tie[3];
            // Y' = luma / 255000; Cb = (1000 B - luma) / (255 * 1772); Cr = (1000 R - luma) / (255 * 1402).
            const long expected[3] = {16 + roundHalfUp(219 * luma, 255000, &tie[0]),
                                      128 + roundHalfUp(224 * (1000 * B - luma), 451860, &tie[1]),
                                      128 + roundHalfUp(224 * (1000 * R - luma), 357510, &tie[2])};
            for (std::size_t k = 0; k < 3; ++k) {
                if (tie[k]) {
                    ++ties;
                } else {
                    wrong += ycc[i * 3 + k] != expected[k];
                }
            }
        }
    }
    std::printf("R'G'B' u8: 16777216 colors to Y'CbCr u8: %ld codes not the exact rounding, %ld exact ties not compared\n",
                wrong, ties);
    return wrong == 0 ? 0 : 1;
}

// The defined code of a double value: c_lo plus round half up of
// (v - lo) (c_hi - c_lo) in double, divided by hi - lo = 1, decided exactly.
int definedCode(double v, double lo, double hi, int c_lo, int c_hi)
{
    if (!(v > lo)) {
        return c_lo;
    }
    if (v >= hi) {
        return c_hi;
    }
    const double twice_scaled = 2.0 * ((v - lo) * (c_hi - c_lo));
    int k = 0;
    int high = c_hi - c_lo;
    while (k < high) {
        const int mid = (k + high + 1) / 2;
        if (twice_scaled >= 2.0 * mid - 1.0) {
            k = mid;
        } else {
            high = mid - 1;
        }
    }
    return c_lo + k;
}

int prophotoColors()
{
    const PixelFormat source = *PixelFormat::fromName("R'G'B' u8-ProPhoto");
    const Converter direct = *Converter::create(source, *PixelFormat::fromName("Y'CbCr u8-ProPhoto"));
    const Converter to_double = *Converter::create(source, *PixelFormat::fromName("Y'CbCr double-ProPhoto"));
    const Converter from_double =
        *Converter::create(*PixelFormat::fromName("Y'CbCr double-ProPhoto"), *PixelFormat::fromName("Y'CbCr u8-ProPhoto"));
    constexpr std::size_t batch = 1u << 16;
    std::vector<std::uint8_t> rgb(batch * 3), ycc(batch * 3), staged(batch * 3);
    std::vector<double> values(batch * 3);
    long mismatches = 0, undefined = 0;
    for (std::size_t r = 0; r < 256; ++r) {
        for (std::size_t i = 0; i < batch; ++i) {
            rgb[i * 3] = static_cast<std::uint8_t>(r);
            rgb[i * 3 + 1] = static_cast<std::uint8_t>(i >> 8);
            rgb[i * 3 + 2] = static_cast<std::uint8_t>(i & 0xFF);
        }
        const auto in = std::as_bytes(std::span(rgb));
        if (!direct.process(in, std::as_writable_bytes(std::span(ycc)), batch) ||
            !to_double.process(in, std::as_writable_bytes(std::span(values)), batch) ||
            !from_double.process(std::as_bytes(std::span(values)), std::as_writable_bytes(std::span(staged)), batch)) {
            std::printf("conversion failed\n");
            return 1;
        }
        for (std::size_t k = 0; k < batch * 3; ++k) {
            const bool luma = k % 3 == 0;
            mismatches += ycc[k] != staged[k];
            undefined += ycc[k] != definedCode(values[k], luma ? 0.0 : -0.5, luma ? 1.0 : 0.5, 16, luma ? 235 : 240);
        }
    }
    std::printf("R'G'B' u8-ProPhoto: 16777216 colors to Y'CbCr u8: %ld differ from the staged route, %ld codes not the "
                "defined rounding\n",
                mismatches, undefined);
    return mismatches == 0 && undefined == 0 ? 0 : 1;
}

// Every binary32 pattern through encodeRange<uint8_t>(x, lo, hi, c_lo, c_hi)
// (hi - lo = 1, lo a multiple of 1/2), against c_lo + k with
// (2k - 1) <= 2 (x - lo) (c_hi - c_lo) < (2k + 1). For |x| >= 2^-12, x * 2^36
// is an integer and the products stay below 2^46. Smaller |x| lie within
// (c_hi - c_lo) 2^-12 <= 0.055 of the integer at x = 0 (16, or 128 for chroma),
// so their code is that integer.
long rangeMismatches(double lo, std::int64_t c_lo, std::int64_t c_hi, std::uint64_t first, std::uint64_t last)
{
    constexpr std::int64_t one = std::int64_t{1} << 36;
    const double hi = lo + 1.0;
    const std::int64_t span = c_hi - c_lo;
    const auto lo_scaled = static_cast<std::int64_t>(lo * 0x1p36);
    const std::int64_t code_at_zero = c_lo + static_cast<std::int64_t>(-lo * static_cast<double>(span)); // exact
    long wrong = 0;
    for (std::uint64_t pattern = first; pattern < last; ++pattern) {
        const auto bits = static_cast<std::uint32_t>(pattern);
        const float x = std::bit_cast<float>(bits);
        const std::int64_t code = chromaroute::detail::encodeRange<std::uint8_t>(x, lo, hi, static_cast<double>(c_lo),
                                                                                 static_cast<double>(c_hi));
        if (!(x > static_cast<float>(lo))) {
            wrong += code != c_lo; // NaN and values at or below lo
            continue;
        }
        if (x >= static_cast<float>(hi)) {
            wrong += code != c_hi;
            continue;
        }
        const std::uint32_t biased_exponent = (bits >> 23) & 0xFF;
        if (biased_exponent < 115) { // |x| < 2^-12
            wrong += code != code_at_zero;
            continue;
        }
        const std::int64_t magnitude = ((std::int64_t{1} << 23) | (bits & 0x7FFFFF)) << (biased_exponent - 114);
        const std::int64_t x_scaled = bits >> 31 ? -magnitude : magnitude; // x * 2^36
        const std::int64_t twice_scaled = 2 * (x_scaled - lo_scaled) * span;
        const std::int64_t k = code - c_lo;
        wrong += !((2 * k - 1) * one <= twice_scaled && twice_scaled < (2 * k + 1) * one);
    }
    return wrong;
}

int rangeEncodings()
{
    constexpr std::uint64_t patterns = std::uint64_t{1} << 32;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<long> wrong[2] = {};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            const std::uint64_t first = patterns * t / threads;
            const std::uint64_t last = patterns * (t + 1) / threads;
            wrong[0] += rangeMismatches(0.0, 16, 235, first, last);
            wrong[1] += rangeMismatches(-0.5, 16, 240, first, last);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("every binary32 input: %ld (u8-luma), %ld (u8-chroma) codes not the exact rounding\n", wrong[0].load(),
                wrong[1].load());
    return wrong[0] == 0 && wrong[1] == 0 ? 0 : 1;
}

} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    int failed = srgbColors();
    failed += prophotoColors();
    failed += rangeEncodings();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("exhaustive Y'CbCr: %s (%.1f s)\n", failed == 0 ? "PASS" : "FAIL", seconds);
    return failed == 0 ? 0 : 1;
}
