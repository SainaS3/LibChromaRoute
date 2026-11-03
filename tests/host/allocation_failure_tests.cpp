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
//
// With lcms2, whose allocations go through the library's handler and its test
// hooks (src/core/cmyk_profile.cpp), rather than operator new:
//
//   chromaroute_allocation_failure_tests lcms2-baseline <file>
//       Writes how many lcms2 blocks a first use that succeeds at once leaves.
//   chromaroute_allocation_failure_tests lcms2-first-touch <file>
//       For n = 1, 2, ...: the n-th lcms2 allocation of the first use (its
//       context and scRGB profile) fails. Each failure throws and leaves no
//       lcms2 block behind; the retry that succeeds leaves the baseline.
//   chromaroute_allocation_failure_tests lcms2-profile
//       The same for opening a CMYK profile and for the transform between two
//       profiles: each failure throws and leaves what was there before.

#include <chromaroute/chromaroute.hpp>

#if CHROMAROUTE_TEST_WITH_LCMS2
#include "core/cmyk_profile.hpp"
#include "support/naive_cmyk_profile.hpp"

#include <fstream>
#endif

#include <algorithm>
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

#if CHROMAROUTE_TEST_WITH_LCMS2

int lcmsBaselineTest(const char* file)
{
    chromaroute::detail::prepareCmykProfiles();
    const long live = chromaroute::detail::liveLcmsAllocations();
    std::ofstream out(file);
    out << live << '\n';
    std::printf("  lcms2 blocks after a first use: %ld\n", live);
    check(live > 0 && out.good(), "the baseline is recorded");
    return failures;
}

int lcmsFirstTouchTest(const char* file)
{
    using namespace chromaroute::detail;
    long baseline = -1;
    std::ifstream in(file);
    in >> baseline;
    check(baseline > 0, "the baseline was recorded (allocation_failure_lcms2-baseline)");

    int injected = 0;
    int leaks = 0;
    bool succeeded = false;
    for (long n = 1; n <= 100000 && !succeeded; ++n) {
        failLcmsAllocation(n);
        bool threw = false;
        try {
            prepareCmykProfiles();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool reached = failLcmsAllocation(0) == 0;
        check(threw == reached, "the first use throws exactly when an lcms2 allocation fails");
        if (threw) {
            ++injected;
            if (liveLcmsAllocations() != 0 && ++leaks <= 3) {
                std::printf("  allocation %ld failed: %ld lcms2 blocks left\n", n, liveLcmsAllocations());
            }
        }
        succeeded = !threw;
    }
    check(leaks == 0, "a failed first use leaves no lcms2 block behind");
    std::printf("  failed first uses injected: %d; after the retry %ld lcms2 blocks (baseline %ld)\n", injected,
                liveLcmsAllocations(), baseline);
    check(succeeded && injected >= 2, "the first use failed at every allocation, then succeeded");
    check(liveLcmsAllocations() == baseline, "the successful retry leaves what a first success leaves");
    prepareCmykProfiles();
    check(liveLcmsAllocations() == baseline, "once made, the first use allocates nothing again");
    const auto space =
        chromaroute::ColorSpace::fromIccProfile("after-retry", chromaroute::test::naiveCmykProfile(2, 3).bytes());
    check(space.has_value() && space->hasCmykProfile(), "a CMYK profile opens after the retry");
    return failures;
}

int lcmsProfileTest()
{
    using chromaroute::ColorSpace;
    using chromaroute::Converter;
    using chromaroute::PixelFormat;
    using namespace chromaroute::detail;
    prepareCmykProfiles();

    // Runs `attempt` with the n-th lcms2 allocation failing, for n = 1, 2, ...,
    // until it needs fewer; returns how many attempts failed.
    const auto exercise = [](const char* what, auto attempt) {
        int injected = 0;
        bool succeeded = false;
        for (long n = 1; n <= 100000 && !succeeded; ++n) {
            const long before = liveLcmsAllocations();
            failLcmsAllocation(n);
            bool threw = false;
            bool made = false;
            try {
                made = attempt();
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            const bool reached = failLcmsAllocation(0) == 0;
            check(threw == reached, what);
            if (threw) {
                ++injected;
                if (liveLcmsAllocations() != before) {
                    std::printf("  %s, allocation %ld failed: %ld lcms2 blocks before, %ld after\n", what, n, before,
                                liveLcmsAllocations());
                }
                check(liveLcmsAllocations() == before, "a failed attempt leaves the lcms2 blocks it found");
            } else {
                check(made, "the attempt succeeds once no allocation fails");
            }
            succeeded = !threw;
        }
        std::printf("  %s: %d failed attempts injected\n", what, injected);
        check(succeeded && injected >= 2, "every allocation failed once, then the attempt succeeded");
        return injected;
    };

    const auto first = chromaroute::test::naiveCmykProfile(2, 3).bytes();
    exercise("opening a CMYK profile", [&] { return ColorSpace::fromIccProfile("oom-first", first).has_value(); });
    check(ColorSpace::fromName("oom-first").has_value(), "the profile's space exists once, after the failures");

    const auto second = ColorSpace::fromIccProfile("oom-second", chromaroute::test::naiveCmykProfile(2, 3, true).bytes());
    check(second.has_value(), "a second profile opens");

    // The same link made with no failure, as the reference: the same tables
    // with a copyright tag added, so their bytes, and so their spaces, differ.
    const std::vector<std::byte> copyright = {std::byte{'t'}, std::byte{'e'}, std::byte{'x'}, std::byte{'t'},
                                              std::byte{0},   std::byte{0},   std::byte{0},   std::byte{0},
                                              std::byte{'r'}, std::byte{0}};
    check(ColorSpace::fromIccProfile("ref-first", chromaroute::test::naiveCmykProfile(2, 3).raw("cprt", copyright).bytes())
                  .has_value() &&
              ColorSpace::fromIccProfile("ref-second",
                                         chromaroute::test::naiveCmykProfile(2, 3, true).raw("cprt", copyright).bytes())
                  .has_value(),
          "the reference profiles open");
    const double inks[4] = {0.5, 0.0, 0.25, 0.25};
    const auto convert = [&](const char* from, const char* to, double (&out)[4]) {
        const auto converter = Converter::create(*PixelFormat::fromName(from), *PixelFormat::fromName(to));
        return converter.has_value() &&
               converter->process(std::as_bytes(std::span(inks)), std::as_writable_bytes(std::span(out)), 1).has_value();
    };
    double expected[4] = {};
    check(convert("CMYK double-ref-first", "CMYK double-ref-second", expected), "the reference link converts");

    const PixelFormat from = *PixelFormat::fromName("CMYK double-oom-first");
    const PixelFormat to = *PixelFormat::fromName("CMYK double-oom-second");
    exercise("linking two profiles", [&] { return Converter::create(from, to).has_value(); });
    double out[4] = {-1, -1, -1, -1};
    const bool converted = convert("CMYK double-oom-first", "CMYK double-oom-second", out);
    const bool same = converted && std::equal(std::begin(out), std::end(out), std::begin(expected));
    if (!same) {
        std::printf("  after the failures C %g M %g Y %g K %g; with none C %g M %g Y %g K %g\n", out[0], out[1],
                    out[2], out[3], expected[0], expected[1], expected[2], expected[3]);
    }
    check(same, "after the failures, the link converts exactly as one made with none");
    return failures;
}

#endif

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
    const std::string_view mode = argc >= 2 ? argv[1] : "";
    int result = 2;
    if (mode == "first-touch") {
        result = firstTouchTest();
    } else if (mode == "plan-cache") {
        result = planCacheTest();
    } else if (mode == "space-variant") {
        result = spaceVariantTest();
    } else if (mode == "concurrent-first-touch") {
        result = concurrentFirstTouchTest();
#if CHROMAROUTE_TEST_WITH_LCMS2
    } else if (mode == "lcms2-baseline" && argc == 3) {
        result = lcmsBaselineTest(argv[2]);
    } else if (mode == "lcms2-first-touch" && argc == 3) {
        result = lcmsFirstTouchTest(argv[2]);
    } else if (mode == "lcms2-profile") {
        result = lcmsProfileTest();
#endif
    } else {
        std::printf("usage: chromaroute_allocation_failure_tests first-touch|plan-cache|space-variant|"
                    "concurrent-first-touch|lcms2-baseline <file>|lcms2-first-touch <file>|lcms2-profile\n");
        return 2;
    }
    std::printf("%s: %s\n", std::string(mode).c_str(), result == 0 ? "PASS" : "FAIL");
    return result == 0 ? 0 : 1;
}
