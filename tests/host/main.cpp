// Host-side (desktop) test runner. Development convenience only: passing here
// says nothing about UWP runtime behavior; see tests/platform/uwp.

#include "support/test_harness.hpp"

#include <cstdio>

int main()
{
    return chromaroute::test::runAll(stdout);
}
