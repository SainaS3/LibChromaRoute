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

#include <chromaroute/chromaroute.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <string_view>
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
    } else {
        std::printf("usage: chromaroute_allocation_failure_tests first-touch|plan-cache\n");
        return 2;
    }
    std::printf("%s: %s\n", std::string(mode).c_str(), result == 0 ? "PASS" : "FAIL");
    return result == 0 ? 0 : 1;
}
