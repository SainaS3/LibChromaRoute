// Exhaustive HSL, HSV and HCY checks, host Release only (docs/testing.md):
//  1. All 16,777,216 "R'G'B' u8" colors in sRGB to "HSL float" and "HSV float".
//     There R'G'B' is already the sRGB-curve encoding, so each component is an
//     exact rational n / d with d <= 6 * 255: hue (G - B) / (6 C) and the like,
//     lightness (max + min) / 510, saturation C / (max + min), C / (510 - max -
//     min) or C / max, with C = max - min. In lowest terms such a value has at
//     most 11 significant bits, so it is never a binary32 midpoint, and it lies
//     at least 2^-36 (relative) from every midpoint. The double quotient n / d is
//     within 2^-53 of it, so rounding that quotient to binary32 gives the exact
//     rounding, which the library must return.
//  2. The same colors to "HCY float": HCY's weights are not exact in double, so
//     direct must equal the staged route through "HCY double"; the hue must be
//     HSV's, bit for bit; chroma 0 must go with hue 0, and luma of a gray is its
//     value.
//  3. Round trips: every color through each model in float and in double comes
//     back to the same u8 code, in every built-in space, with one
//     measured exception. ACES2065-1's blue luma weight is negative, so at the
//     green-led hue whose middle channel is red at 0.767 the peak luma of HCY is
//     1, and bright colors near it have both 1 - Y and 1 - peak near 1e-6. HCY's
//     chroma scale is their ratio, so binary32 rounding of Y moves the recovered
//     chroma by up to a few percent there: R'G'B' (227, 255, 148) comes back as
//     (227, 255, 146) through "HCY float". That route is allowed at most 1
//     changed code, by at most 2; in double every color round-trips.

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace {

constexpr std::size_t batch = 1u << 16; // one red value, every green and blue

Converter converter(const std::string& from, const std::string& to)
{
    return *Converter::create(*PixelFormat::fromName(from), *PixelFormat::fromName(to));
}

template <typename From, typename To>
bool run(const Converter& c, const std::vector<From>& in, std::vector<To>& out)
{
    return c.process(std::as_bytes(std::span(in)), std::as_writable_bytes(std::span(out)), batch).has_value();
}

float quotient(std::int64_t n, std::int64_t d)
{
    return static_cast<float>(static_cast<double>(n) / static_cast<double>(d));
}

struct Exact {
    float hsl[3];
    float hsv[3];
};

Exact exactHslHsv(std::int64_t r, std::int64_t g, std::int64_t b)
{
    const std::int64_t high = std::max({r, g, b}), low = std::min({r, g, b});
    const std::int64_t chroma = high - low, sum = high + low;
    Exact e{{0.0f, 0.0f, quotient(sum, 510)}, {0.0f, 0.0f, quotient(high, 255)}};
    if (chroma == 0) {
        return e;
    }
    std::int64_t sextant; // hue * 6 C
    if (high == r) {
        sextant = g - b < 0 ? g - b + 6 * chroma : g - b;
    } else if (high == g) {
        sextant = 2 * chroma + b - r;
    } else {
        sextant = 4 * chroma + r - g;
    }
    const float hue = quotient(sextant, 6 * chroma);
    e.hsl[0] = e.hsv[0] = hue;
    e.hsl[1] = quotient(chroma, sum <= 255 ? sum : 510 - sum);
    e.hsv[1] = quotient(chroma, high);
    return e;
}

void fill(std::vector<std::uint8_t>& rgb, std::size_t r)
{
    for (std::size_t i = 0; i < batch; ++i) {
        rgb[i * 3] = static_cast<std::uint8_t>(r);
        rgb[i * 3 + 1] = static_cast<std::uint8_t>(i >> 8);
        rgb[i * 3 + 2] = static_cast<std::uint8_t>(i & 0xFF);
    }
}

struct Counts {
    std::atomic<long> hsl_wrong{0}, hsv_wrong{0}, hcy_staged{0}, hcy_rules{0}, round_trips{0}, failures{0};
    std::atomic<long> ill_conditioned_codes{0}, ill_conditioned_worst{0}; // HCY float in ACES2065-1
};

void sweep(std::size_t first_red, std::size_t step, Counts& counts)
{
    const Converter to_hsl = converter("R'G'B' u8", "HSL float");
    const Converter to_hsv = converter("R'G'B' u8", "HSV float");
    const Converter to_hcy = converter("R'G'B' u8", "HCY float");
    const Converter to_hcy_double = converter("R'G'B' u8", "HCY double");
    const Converter hcy_narrow = converter("HCY double", "HCY float");
    struct Trip {
        Converter there, back;
        bool to_float;
        std::string name;
        bool ill_conditioned; // HCY float in ACES2065-1, see 3. above
    };
    std::vector<Trip> trips;
    // Every built-in space. ACES2065-1's luminance row has a negative blue
    // weight, so HCY's peak passes 0 and 1 at some hues there.
    for (const char* space : {"", "-scRGB", "-Rec2020", "-DisplayP3", "-Adobish", "-ProPhoto", "-Apple", "-ACEScg",
                              "-ACES2065-1"}) {
        for (const char* model : {"HSL", "HSV", "HCY"}) {
            for (const char* type : {" float", " double"}) {
                const std::string rgb = std::string("R'G'B' u8") + space;
                const std::string values = std::string(model) + type + space;
                trips.push_back({converter(rgb, values), converter(values, rgb), type[1] == 'f', values,
                                 values == "HCY float-ACES2065-1"});
            }
        }
    }
    std::vector<std::uint8_t> rgb(batch * 3), back(batch * 3);
    std::vector<float> hsl(batch * 3), hsv(batch * 3), hcy(batch * 3), staged(batch * 3), floats(batch * 3);
    std::vector<double> doubles(batch * 3);
    for (std::size_t r = first_red; r < 256; r += step) {
        fill(rgb, r);
        if (!run(to_hsl, rgb, hsl) || !run(to_hsv, rgb, hsv) || !run(to_hcy, rgb, hcy) ||
            !run(to_hcy_double, rgb, doubles) || !run(hcy_narrow, doubles, staged)) {
            ++counts.failures;
            return;
        }
        long hsl_wrong = 0, hsv_wrong = 0, hcy_staged = 0, hcy_rules = 0, round_trips = 0, ill_conditioned_codes = 0;
        for (std::size_t i = 0; i < batch; ++i) {
            const std::int64_t R = rgb[i * 3], G = rgb[i * 3 + 1], B = rgb[i * 3 + 2];
            const Exact e = exactHslHsv(R, G, B);
            for (std::size_t k = 0; k < 3; ++k) {
                hsl_wrong += std::bit_cast<std::uint32_t>(hsl[i * 3 + k]) != std::bit_cast<std::uint32_t>(e.hsl[k]);
                hsv_wrong += std::bit_cast<std::uint32_t>(hsv[i * 3 + k]) != std::bit_cast<std::uint32_t>(e.hsv[k]);
                hcy_staged += std::bit_cast<std::uint32_t>(hcy[i * 3 + k]) != std::bit_cast<std::uint32_t>(staged[i * 3 + k]);
            }
            const float hue = hcy[i * 3], chroma = hcy[i * 3 + 1], luma = hcy[i * 3 + 2];
            const bool gray = R == G && G == B;
            hcy_rules += std::bit_cast<std::uint32_t>(hue) != std::bit_cast<std::uint32_t>(hsv[i * 3]) ||
                         !(hue >= 0.0f && hue < 1.0f) || (chroma == 0.0f) != gray ||
                         (gray && luma != quotient(R, 255));
        }
        for (const Trip& trip : trips) {
            const bool ok = trip.to_float ? run(trip.there, rgb, floats) && run(trip.back, floats, back)
                                          : run(trip.there, rgb, doubles) && run(trip.back, doubles, back);
            if (!ok) {
                ++counts.failures;
                return;
            }
            for (std::size_t k = 0; k < rgb.size(); ++k) {
                if (rgb[k] == back[k]) {
                    continue;
                }
                long& changed = trip.ill_conditioned ? ill_conditioned_codes : round_trips;
                if (trip.ill_conditioned) {
                    const long deviation = std::abs(static_cast<long>(rgb[k]) - static_cast<long>(back[k]));
                    long worst = counts.ill_conditioned_worst.load();
                    while (deviation > worst && !counts.ill_conditioned_worst.compare_exchange_weak(worst, deviation)) {
                    }
                }
                if (++changed <= 5) {
                    const std::size_t p = k / 3;
                    std::printf("  %s: R'G'B' (%d, %d, %d) came back as (%d, %d, %d)\n", trip.name.c_str(),
                                rgb[p * 3], rgb[p * 3 + 1], rgb[p * 3 + 2], back[p * 3], back[p * 3 + 1],
                                back[p * 3 + 2]);
                }
            }
        }
        counts.hsl_wrong += hsl_wrong;
        counts.hsv_wrong += hsv_wrong;
        counts.hcy_staged += hcy_staged;
        counts.hcy_rules += hcy_rules;
        counts.round_trips += round_trips;
        counts.ill_conditioned_codes += ill_conditioned_codes;
    }
}

} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    Counts counts;
    const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] { sweep(t, threads, counts); });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::printf("R'G'B' u8: 16777216 colors: %ld HSL float and %ld HSV float components not the exact rounding\n",
                counts.hsl_wrong.load(), counts.hsv_wrong.load());
    std::printf("HCY float: %ld components differ from the staged route, %ld colors break the hue or gray rules\n",
                counts.hcy_staged.load(), counts.hcy_rules.load());
    std::printf("round trips through HSL, HSV, HCY in float and double, all 9 built-in spaces: %ld codes "
                "changed; HCY float in ACES2065-1: %ld codes changed, by at most %ld (allowed: 1, by 2)\n",
                counts.round_trips.load(), counts.ill_conditioned_codes.load(), counts.ill_conditioned_worst.load());
    const bool pass = counts.failures == 0 && counts.hsl_wrong == 0 && counts.hsv_wrong == 0 && counts.hcy_staged == 0 &&
                      counts.hcy_rules == 0 && counts.round_trips == 0 && counts.ill_conditioned_codes <= 1 &&
                      counts.ill_conditioned_worst <= 2;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("exhaustive HSL, HSV, HCY: %s (%.1f s)\n", pass ? "PASS" : "FAIL", seconds);
    return pass ? 0 : 1;
}
