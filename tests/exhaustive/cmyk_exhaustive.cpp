// Exhaustive CMYK checks, host Release only (docs/testing.md):
//  1. All 16,777,216 "R'G'B' u8-scRGB" colors (linear codes on CMYK's own RGB
//     side, S5) to "CMYK u8" and "cmyk u8". With m = max(R, G, B) > 0 the
//     inverted form is (R / m, G / m, B / m, m / 255), so each code is round half
//     up of 255 R / m (inks: 255 (m - R) / m) and K is 255 - m (inverted: m),
//     decided in integers; black is (255, 255, 255, 255), inverted 0. A value
//     exactly halfway between two codes is a tie that double evaluation may
//     round either way (255 / 6 = 42.5, but 1/255 divided by 6/255 is not 1/6
//     in double); such cases are counted and may take either neighbor. Every
//     other value lies at least 1 / (2 m) >= 1 / 510 from a tie, far beyond
//     double's error.
//  2. Round trips: every R'G'B' u8 color, in sRGB, ProPhoto and ACES2065-1,
//     through CMYK and cmyk in float and double comes back to the same code;
//     with alpha 77 so do R'G'B'A u8 colors through CaMaYaKaA and camayakaA
//     float.

#include <chromaroute/chromaroute.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

// floor(n / d + 1/2) for d > 0 and n >= 0; *tie is set when n / d is exactly halfway.
long roundHalfUp(std::int64_t n, std::int64_t d, bool* tie)
{
    const std::int64_t twice = 2 * n + d;
    *tie = twice % (2 * d) == 0;
    return static_cast<long>(twice / (2 * d));
}

struct Counts {
    std::atomic<long> wrong{0}, ties{0}, round_trips{0}, failures{0};
};

void sweep(std::size_t first_red, std::size_t step, Counts& counts)
{
    const Converter to_inks = converter("R'G'B' u8-scRGB", "CMYK u8");
    const Converter to_inverted = converter("R'G'B' u8-scRGB", "cmyk u8");
    struct Trip {
        Converter there, back;
        int kind; // 0 float, 1 double, 2 float with alpha
        std::string name;
    };
    std::vector<Trip> trips;
    for (const char* space : {"", "-ProPhoto", "-ACES2065-1"}) {
        for (const char* model : {"CMYK", "cmyk"}) {
            for (const char* type : {" float", " double"}) {
                const std::string rgb = std::string("R'G'B' u8") + space;
                const std::string values = std::string(model) + type + space;
                trips.push_back({converter(rgb, values), converter(values, rgb), type[1] == 'f' ? 0 : 1, values});
            }
        }
    }
    for (const char* model : {"CaMaYaKaA float", "camayakaA float"}) {
        trips.push_back({converter("R'G'B'A u8", model), converter(model, "R'G'B'A u8"), 2, model});
    }
    std::vector<std::uint8_t> rgb(batch * 3), rgba(batch * 4), inks(batch * 4), inverted(batch * 4), back(batch * 4);
    std::vector<float> floats(batch * 5);
    std::vector<double> doubles(batch * 4);
    for (std::size_t r = first_red; r < 256; r += step) {
        for (std::size_t i = 0; i < batch; ++i) {
            const std::uint8_t c[4] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(i >> 8),
                                       static_cast<std::uint8_t>(i & 0xFF), 77};
            std::copy(c, c + 3, &rgb[i * 3]);
            std::copy(c, c + 4, &rgba[i * 4]);
        }
        if (!run(to_inks, rgb, inks) || !run(to_inverted, rgb, inverted)) {
            ++counts.failures;
            return;
        }
        long wrong = 0, ties = 0, round_trips = 0;
        for (std::size_t i = 0; i < batch; ++i) {
            const std::int64_t m = std::max({rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]});
            for (std::size_t k = 0; k < 3; ++k) {
                const std::int64_t x = rgb[i * 3 + k];
                bool tie_ink = false, tie_inverted = false;
                const long ink = m == 0 ? 255 : roundHalfUp(255 * (m - x), m, &tie_ink);
                const long inv = m == 0 ? 0 : roundHalfUp(255 * x, m, &tie_inverted);
                // At a tie, round half up gives the upper code; either neighbor is accepted.
                ties += tie_ink + tie_inverted;
                wrong += inks[i * 4 + k] != ink && !(tie_ink && inks[i * 4 + k] == ink - 1);
                wrong += inverted[i * 4 + k] != inv && !(tie_inverted && inverted[i * 4 + k] == inv - 1);
            }
            wrong += inks[i * 4 + 3] != 255 - m;
            wrong += inverted[i * 4 + 3] != m;
        }
        for (const Trip& trip : trips) {
            const std::vector<std::uint8_t>& in = trip.kind == 2 ? rgba : rgb;
            const bool ok = trip.kind == 1 ? run(trip.there, in, doubles) && run(trip.back, doubles, back)
                                           : run(trip.there, in, floats) && run(trip.back, floats, back);
            if (!ok) {
                ++counts.failures;
                return;
            }
            for (std::size_t k = 0; k < in.size(); ++k) {
                if (in[k] != back[k] && ++round_trips <= 5) {
                    const std::size_t channels = trip.kind == 2 ? 4 : 3;
                    const std::size_t p = k / channels;
                    std::printf("  %s: R'G'B' (%d, %d, %d) came back as (%d, %d, %d)\n", trip.name.c_str(),
                                in[p * channels], in[p * channels + 1], in[p * channels + 2], back[p * channels],
                                back[p * channels + 1], back[p * channels + 2]);
                }
            }
        }
        counts.wrong += wrong;
        counts.ties += ties;
        counts.round_trips += round_trips;
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
    std::printf("R'G'B' u8-scRGB: 16777216 colors to CMYK u8 and cmyk u8: %ld codes not the exact rounding, %ld exact "
                "ties (either neighbor accepted)\n",
                counts.wrong.load(), counts.ties.load());
    std::printf("round trips through CMYK and cmyk (float, double; sRGB, ProPhoto, ACES2065-1) and the associated "
                "models: %ld codes changed\n",
                counts.round_trips.load());
    const bool pass = counts.failures == 0 && counts.wrong == 0 && counts.round_trips == 0;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("exhaustive CMYK: %s (%.1f s)\n", pass ? "PASS" : "FAIL", seconds);
    return pass ? 0 : 1;
}
