// Behavior when memory runs out. Host only: the global operator new is
// replaced to fail on demand, which must not leak into the shared suite, and
// each mode needs a fresh process.
//
//   chromaroute_allocation_failure_tests first-touch
//       Every allocation fails before anything has built the descriptor
//       registry. The factories throw std::bad_alloc, and work afterwards.
//   chromaroute_allocation_failure_tests plan-cache
//       For n = 1, 2, ...: the n-th allocation inside Converter::create fails.
//       Nothing half-built may remain: the next attempt for that pair must
//       succeed and convert correctly.
//   chromaroute_allocation_failure_tests space-variant
//       The same for PixelFormat::withSpace, each time for a format and space
//       not combined before: afterwards there is exactly one variant.
//   chromaroute_allocation_failure_tests concurrent-first-touch
//       No failures: eight threads make the first registry calls of the
//       process at the same time and must get the same interned descriptors.

#include <chromaroute/chromaroute.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> fail_all{false};
std::atomic<long> fail_countdown{0}; // > 0: the allocation that brings it to 0 fails

void* allocate(std::size_t size)
{
    if (fail_all.load() || (fail_countdown.load() > 0 && --fail_countdown == 0)) {
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        ++failures;
        std::printf("  FAILED: %s\n", what);
    }
}

int firstTouchTest()
{
    fail_all = true; // nothing has built the registry yet
    int threw = 0;
    try {
        (void)chromaroute::ColorSpace::srgb();
    } catch (const std::bad_alloc&) {
        ++threw;
    }
    try {
        (void)chromaroute::TransferFunction::srgb();
    } catch (const std::bad_alloc&) {
        ++threw;
    }
    try {
        (void)chromaroute::TransferFunction::linear();
    } catch (const std::bad_alloc&) {
        ++threw;
    }
    fail_all = false;
    check(threw == 3, "factories throw std::bad_alloc");

    // Nothing was left half-built.
    check(chromaroute::ColorSpace::srgb().name() == "sRGB", "factory afterwards");
    const auto& w = chromaroute::ColorSpace::srgb().luminance();
    check(w[0] > 0.2224 && w[0] < 0.2225 && w[1] > 0.7169 && w[1] < 0.71691, "luminance afterwards");
    return failures;
}

int planCacheTest()
{
    using chromaroute::Converter;
    using chromaroute::PixelFormat;

    // Look the formats up first: that allocates too, and is not under test.
    const PixelFormat source = *PixelFormat::fromName("RGBA float");
    std::vector<PixelFormat> targets;
    for (const char* model : {"Y double", "YA double"}) {
        for (const char* space : {"", "-scRGB", "-Rec2020", "-DisplayP3", "-Adobish", "-ProPhoto", "-Apple", "-ACEScg",
                                  "-ACES2065-1"}) {
            targets.push_back(*PixelFormat::fromName(std::string(model) + space));
        }
    }
    (void)Converter::create(source, *PixelFormat::fromName("RGBA double")); // warm up the cache's own state

    const float rgba[4] = {0.5f, 0.25f, 1.0f, 0.5f};
    const auto& w = chromaroute::ColorSpace::srgb().luminance();
    const double expected_y = w[0] * 0.5 + w[1] * 0.25 + w[2] * 1.0; // the source space's luminance (N7)

    int failed_creations = 0;
    bool covered_all = false;
    for (std::size_t n = 1; n <= targets.size() && !covered_all; ++n) {
        const PixelFormat target = targets[n - 1];
        fail_countdown = static_cast<long>(n);
        bool threw = false;
        try {
            (void)Converter::create(source, target);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool reached = fail_countdown.load() == 0;
        fail_countdown = 0;
        covered_all = !reached; // creation needs fewer than n allocations
        failed_creations += threw;
        check(threw == reached, "creation throws exactly when its allocation fails");

        const auto retry = Converter::create(source, target);
        check(retry.has_value(), "creation succeeds after a failed attempt");
        if (!retry) {
            std::printf("  pair %zu (%s): %s\n", n, std::string(target.name()).c_str(), retry.error().message.c_str());
            continue;
        }
        double out[2] = {-1, -1};
        const bool processed = static_cast<bool>(
            retry->process(std::as_bytes(std::span(rgba)), std::as_writable_bytes(std::span(out)), 1));
        check(processed && out[0] == expected_y && (target.componentCount() == 1 || out[1] == 0.5),
              "the retried conversion is correct");
    }
    std::printf("  failed creations injected: %d\n", failed_creations);
    check(covered_all, "every allocation position of create was exercised");
    check(failed_creations >= 2, "at least two allocation positions were exercised");
    return failures;
}

int spaceVariantTest()
{
    using chromaroute::ColorSpace;
    using chromaroute::PixelFormat;

    std::vector<std::pair<PixelFormat, ColorSpace>> pairs;
    for (const char* base : {"RGBA float", "CIE Lab u8", "Y' u16", "CIE L alpha float"}) {
        for (const char* space : {"scRGB", "Rec2020", "DisplayP3", "Adobish", "ProPhoto", "Apple", "ACEScg", "ACES2065-1"}) {
            pairs.emplace_back(*PixelFormat::fromName(base), *ColorSpace::fromName(space));
        }
    }
    int failed_creations = 0;
    bool covered_all = false;
    for (std::size_t n = 1; n <= pairs.size() && !covered_all; ++n) {
        const auto& [base, space] = pairs[n - 1];
        fail_countdown = static_cast<long>(n);
        bool threw = false;
        try {
            (void)base.withSpace(space);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool reached = fail_countdown.load() == 0;
        fail_countdown = 0;
        covered_all = !reached; // the variant needs fewer than n allocations
        failed_creations += threw;
        check(threw == reached, "withSpace throws exactly when its allocation fails");

        const PixelFormat variant = base.withSpace(space);
        const auto by_name = PixelFormat::fromName(std::string(base.name()) + "-" + std::string(space.name()));
        check(variant == base.withSpace(space) && by_name.has_value() && *by_name == variant,
              "one variant after a failed attempt");
        check(variant.space() == space && variant.componentCount() == base.componentCount() &&
                  variant.bytesPerPixel() == base.bytesPerPixel(),
              "the variant keeps the base layout");
    }
    std::printf("  failed creations injected: %d\n", failed_creations);
    check(covered_all, "every allocation position of withSpace was exercised");
    check(failed_creations >= 2, "at least two allocation positions were exercised");
    return failures;
}

int concurrentFirstTouchTest()
{
    using chromaroute::ColorSpace;
    using chromaroute::Converter;
    using chromaroute::PixelFormat;
    using chromaroute::TransferFunction;

    constexpr int thread_count = 8;
    struct Seen {
        const void* space = nullptr;
        const void* format = nullptr;
        float lab[4] = {};
    };
    std::vector<Seen> seen(thread_count);
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            ++ready;
            while (ready.load() < thread_count) {
                // start together
            }
            Seen& s = seen[t];
            const TransferFunction trc = *TransferFunction::gamma(2.4);
            const ColorSpace space = *ColorSpace::fromChromaticities("", ColorSpace::srgb().chromaticities(), trc);
            const PixelFormat format = PixelFormat::fromName("R'G'B'A u8")->withSpace(space);
            s.space = &space.descriptor();
            s.format = &format.descriptor();
            const auto converter = Converter::create(format, *PixelFormat::fromName("CIE Lab alpha float-Rec2020"));
            const unsigned char pixel[4] = {200, 100, 50, 255};
            if (converter) {
                (void)converter->process(std::as_bytes(std::span(pixel)), std::as_writable_bytes(std::span(s.lab)), 1);
            }
        });
    }
    // The main thread only waits: the workers make the first registry calls.
    for (std::thread& thread : threads) {
        thread.join();
    }
    bool same = true;
    for (const Seen& s : seen) {
        same = same && s.space == seen[0].space && s.format == seen[0].format;
        for (int i = 0; i < 4; ++i) {
            same = same && s.lab[i] == seen[0].lab[i];
        }
    }
    check(same, "every thread gets the same descriptors and results");
    check(seen[0].lab[0] > 0.0f && seen[0].lab[3] == 1.0f, "the conversion ran");
    return failures;
}

} // namespace

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep the progress if the process dies
    const std::string_view mode = argc == 2 ? argv[1] : "";
    int result = 2;
    if (mode == "first-touch") {
        result = firstTouchTest();
    } else if (mode == "plan-cache") {
        result = planCacheTest();
    } else if (mode == "space-variant") {
        result = spaceVariantTest();
    } else if (mode == "concurrent-first-touch") {
        result = concurrentFirstTouchTest();
    } else {
        std::printf("usage: chromaroute_allocation_failure_tests first-touch|plan-cache|space-variant|"
                    "concurrent-first-touch\n");
        return 2;
    }
    std::printf("%s: %s\n", std::string(mode).c_str(), result == 0 ? "PASS" : "FAIL");
    return result == 0 ? 0 : 1;
}
