#pragma once

// Helpers shared by the color-model tests: one-call conversions, oracle
// comparisons, and exact predicates for integer range encodings.

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

namespace chromaroute::test {

// Converts whole pixels of `input` from one named format to another. On a
// failure the result gets an extra element, so any comparison fails.
template <typename To, typename From>
std::vector<To> convert(const char* from, const char* to, const std::vector<From>& input)
{
    const Converter converter = *Converter::create(*PixelFormat::fromName(from), *PixelFormat::fromName(to));
    const std::size_t pixels = input.size() * sizeof(From) / converter.sourceFormat().bytesPerPixel();
    std::vector<To> output(pixels * converter.destinationFormat().bytesPerPixel() / sizeof(To));
    if (!converter.process(std::as_bytes(std::span(input)), std::as_writable_bytes(std::span(output)), pixels)) {
        output.push_back(To{});
    }
    return output;
}

// Oracle u8 triples packed as R | G << 8 | B << 16, unpacked to bytes.
inline std::vector<std::uint8_t> unpackRgb(std::span<const std::uint32_t> packed)
{
    std::vector<std::uint8_t> rgb;
    for (std::uint32_t p : packed) {
        rgb.insert(rgb.end(), {static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(p >> 8),
                               static_cast<std::uint8_t>(p >> 16)});
    }
    return rgb;
}

inline std::vector<float> floats(std::span<const std::uint32_t> bits)
{
    std::vector<float> out;
    for (std::uint32_t b : bits) {
        out.push_back(std::bit_cast<float>(b));
    }
    return out;
}

// binary32 results that differ from the oracle's bit patterns; the first few are logged.
inline int bitMismatches(const std::vector<float>& got, std::span<const std::uint32_t> expected, std::FILE* log)
{
    if (got.size() != expected.size()) {
        return 1 << 20;
    }
    int mismatches = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::bit_cast<std::uint32_t>(got[i]) != expected[i] && ++mismatches <= 5) {
            std::fprintf(log, "  [%zu] got %a expected %a\n", i, static_cast<double>(got[i]),
                         static_cast<double>(std::bit_cast<float>(expected[i])));
        }
    }
    return mismatches;
}

// Integer results that differ from the oracle's codes; the first few are logged.
template <typename T>
int codeMismatches(const std::vector<T>& got, std::span<const std::uint32_t> expected, std::FILE* log)
{
    if (got.size() != expected.size()) {
        return 1 << 20;
    }
    int mismatches = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i] != expected[i] && ++mismatches <= 5) {
            std::fprintf(log, "  [%zu] got %u expected %u\n", i, static_cast<unsigned>(got[i]),
                         static_cast<unsigned>(expected[i]));
        }
    }
    return mismatches;
}

// The exact code a binary32 value gets in an integer range encoding (codes
// c_lo..c_hi for values lo..hi): c_lo plus round half up of
// (x - lo) (c_hi - c_lo) / (hi - lo). Decided in 64-bit integers on x's
// mantissa and exponent, not with the library's double arithmetic. lo must be
// a multiple of 1/2 and hi - lo an integer; every value the tests use has
// |x| >= 2^-11, so x * 2^35 is an integer, and the products stay below 2^61.
inline long exactRangeCode(float x, double lo, double hi, long c_lo, long c_hi)
{
    if (!(x > lo)) {
        return c_lo;
    }
    if (x >= hi) {
        return c_hi;
    }
    int exponent = 0;
    const double fraction = std::frexp(static_cast<double>(x), &exponent); // x = fraction * 2^exponent
    const int shift = exponent - 24 + 35;
    if (shift < 0) {
        return -1; // outside what the tests use: fails the comparison
    }
    const std::int64_t one = std::int64_t{1} << 35;
    const auto mantissa = static_cast<std::int64_t>(std::ldexp(fraction, 24));
    const auto range = static_cast<std::int64_t>(hi - lo);
    const auto lo_scaled = static_cast<std::int64_t>(lo * 0x1p35);
    const std::int64_t span = c_hi - c_lo;
    const std::int64_t twice_scaled = 2 * (mantissa * (std::int64_t{1} << shift) - lo_scaled) * span; // times 2^35
    long code = 0;
    long high = static_cast<long>(span);
    while (code < high) { // largest k with 2 * scaled >= (2k - 1) * range, by bisection
        const long mid = (code + high + 1) / 2;
        if (twice_scaled >= (2 * std::int64_t{mid} - 1) * range * one) {
            code = mid;
        } else {
            high = mid - 1;
        }
    }
    return c_lo + code;
}

// The defined code of a double value (docs/architecture.md, integer range
// types): s = (v - lo) (c_hi - c_lo) evaluated in double, then c_lo plus round
// half up of s / (hi - lo), decided with exact comparisons (2 s and
// (2k - 1)(hi - lo) are exact in double).
inline long definedRangeCode(double v, double lo, double hi, long c_lo, long c_hi)
{
    if (!(v > lo)) {
        return c_lo;
    }
    if (v >= hi) {
        return c_hi;
    }
    const double twice_scaled = 2.0 * ((v - lo) * static_cast<double>(c_hi - c_lo));
    long code = 0;
    long high = c_hi - c_lo;
    while (code < high) {
        const long mid = (code + high + 1) / 2;
        if (twice_scaled >= (2.0 * mid - 1.0) * (hi - lo)) {
            code = mid;
        } else {
            high = mid - 1;
        }
    }
    return c_lo + code;
}

// True when d is a double nearest to n / den, for integers n and den: the
// residuals d * den - n are exact under fma here, and neither neighbor of d
// has a smaller one.
inline bool nearestQuotient(double d, double n, double den)
{
    const auto residual = [&](double x) { return std::fabs(std::fma(x, den, -n)); };
    const double r = residual(d);
    return r <= residual(std::nextafter(d, -1e300)) && r <= residual(std::nextafter(d, 1e300));
}

inline int bitsDiffer(double a, double b)
{
    return std::bit_cast<std::uint64_t>(a) != std::bit_cast<std::uint64_t>(b);
}

// The larger of two errors, for a worst-case check. A NaN or infinite error
// (a failed sample) makes the result infinite and keeps it so; std::max would
// silently drop a NaN.
inline double worstOf(double worst, double error) noexcept
{
    return std::isfinite(error) ? std::max(worst, error) : std::numeric_limits<double>::infinity();
}

} // namespace chromaroute::test
