#include "support/test_harness.hpp"

#include <chromaroute/chromaroute.hpp>

CR_TEST(library_version_matches_project_version)
{
    const chromaroute::Version version = chromaroute::libraryVersion();
    CR_CHECK(version.major == CHROMAROUTE_TEST_EXPECTED_VERSION_MAJOR);
    CR_CHECK(version.minor == CHROMAROUTE_TEST_EXPECTED_VERSION_MINOR);
    CR_CHECK(version.patch == CHROMAROUTE_TEST_EXPECTED_VERSION_PATCH);
}
