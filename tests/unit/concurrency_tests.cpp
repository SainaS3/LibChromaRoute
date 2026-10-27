// One Converter used from many threads at once, and converters created
// concurrently (plan cache), must give the single-threaded results.

#include "support/test_harness.hpp"

#include <chromaroute/chromaroute.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using chromaroute::Converter;
using chromaroute::PixelFormat;

namespace {

constexpr int thread_count = 8;
constexpr std::size_t pixels = 4096;

std::vector<std::uint8_t> pattern()
{
    std::vector<std::uint8_t> bytes(pixels * 4);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * 29 + 7) & 0xFF);
    }
    return bytes;
}

} // namespace

CR_TEST(shared_converter_is_thread_safe)
{
    const Converter decode =
        *Converter::create(*PixelFormat::fromName("R'G'B'A u8"), *PixelFormat::fromName("RGBA float"));
    const std::vector<std::uint8_t> source = pattern();
    std::vector<float> reference(pixels * 4);
    CR_CHECK(decode.process(std::as_bytes(std::span(source)), std::as_writable_bytes(std::span(reference)), pixels)
                 .has_value());

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&] {
            std::vector<float> out(pixels * 4);
            for (int round = 0; round < 20; ++round) {
                const auto status = decode.process(std::as_bytes(std::span(source)),
                                                   std::as_writable_bytes(std::span(out)), pixels);
                if (!status || out != reference) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CR_CHECK(failures.load() == 0);
}

CR_TEST(concurrent_creation_yields_equivalent_converters)
{
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            const char* from = t % 2 == 0 ? "R'G'B'A u8" : "RGBA float";
            const char* to = t % 2 == 0 ? "RGBA float" : "R'G'B'A u8";
            for (int round = 0; round < 200; ++round) {
                const auto converter = Converter::create(*PixelFormat::fromName(from), *PixelFormat::fromName(to));
                if (!converter || converter->sourceFormat().name() != from) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CR_CHECK(failures.load() == 0);
}
