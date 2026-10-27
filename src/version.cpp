#include <chromaroute/version.hpp>

namespace chromaroute {

Version libraryVersion() noexcept
{
    return Version{CHROMAROUTE_VERSION_MAJOR, CHROMAROUTE_VERSION_MINOR, CHROMAROUTE_VERSION_PATCH};
}

} // namespace chromaroute
