#include "support/test_harness.hpp"

#include <exception>
#include <vector>

namespace chromaroute::test {
namespace {

struct TestCase {
    const char* name;
    TestFunction function;
};

// Function-local static avoids static-initialization-order problems between
// registrars in different translation units.
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

} // namespace

void Context::check(bool ok, const char* expression, const char* file, int line) noexcept
{
    if (!ok) {
        ++failures_;
        std::fprintf(out_, "  CHECK FAILED: %s (%s:%d)\n", expression, file, line);
    }
}

Registrar::Registrar(const char* name, TestFunction function)
{
    registry().push_back(TestCase{name, function});
}

int runAll(std::FILE* out)
{
    int failed = 0;
    for (const TestCase& test : registry()) {
        Context context(out);
        bool threw = false;
        try {
            test.function(context);
        } catch (const std::exception& error) {
            threw = true;
            std::fprintf(out, "  EXCEPTION: %s\n", error.what());
        } catch (...) {
            threw = true;
            std::fprintf(out, "  EXCEPTION: unknown\n");
        }
        const bool passed = !threw && context.failures() == 0;
        if (!passed) {
            ++failed;
        }
        std::fprintf(out, "[%s] %s\n", passed ? "PASS" : "FAIL", test.name);
        std::fflush(out); // keep the log if a later test crashes the process
    }
    std::fprintf(out, "%zu test(s), %d failed\n", registry().size(), failed);
    std::fflush(out);
    return failed == 0 && !registry().empty() ? 0 : 1;
}

} // namespace chromaroute::test
