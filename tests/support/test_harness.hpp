#pragma once

// Minimal dependency-free test harness. Runners pass the output stream
// explicitly because the packaged UWP runner cannot use stdout.

#include <cstdio>

namespace chromaroute::test {

class Context {
public:
    explicit Context(std::FILE* out) noexcept : out_(out) {}

    void check(bool ok, const char* expression, const char* file, int line) noexcept;

    [[nodiscard]] int failures() const noexcept { return failures_; }
    [[nodiscard]] std::FILE* out() const noexcept { return out_; }

private:
    std::FILE* out_;
    int failures_ = 0;
};

using TestFunction = void (*)(Context&);

struct Registrar {
    Registrar(const char* name, TestFunction function);
};

// Runs every registered test and writes a log to `out`.
// Returns 0 when all tests pass and 1 otherwise.
int runAll(std::FILE* out);

} // namespace chromaroute::test

#define CR_TEST(name)                                                              \
    static void name(::chromaroute::test::Context& cr_context);                   \
    static const ::chromaroute::test::Registrar name##_registrar{#name, &name};   \
    static void name(::chromaroute::test::Context& cr_context)

#define CR_CHECK(expression) \
    cr_context.check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
