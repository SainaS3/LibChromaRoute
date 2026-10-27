#pragma once

namespace chromaroute {

struct Version {
    int major;
    int minor;
    int patch;
};

// Version of the compiled library (may differ from the headers in use).
[[nodiscard]] Version libraryVersion() noexcept;

} // namespace chromaroute
