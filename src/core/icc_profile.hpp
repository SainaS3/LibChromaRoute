#pragma once

// The matrix/TRC parts of ICC profiles (ICC.1, versions 2 and 4), read with
// every offset and length checked against the profile's bytes.

#include <chromaroute/color_space.hpp>
#include <chromaroute/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace chromaroute::detail {

// One tone curve tag: curveType ("curv") or parametricCurveType ("para").
struct IccCurve {
    enum class Kind : std::uint8_t {
        identity,   // curveType with no entries
        gamma,      // curveType with one entry, a u8Fixed8 exponent
        sampled,    // curveType with 2 or more uInt16 entries, as entry / 65535
        parametric, // parametricCurveType, s15Fixed16 parameters
    };
    Kind kind = Kind::identity;
    double gamma = 0.0;
    std::vector<double> samples;
    ParametricCurve parametric{};
};

struct IccMatrixProfile {
    bool gray = false;
    Matrix3 rgb_to_xyz{};        // RGB: the colorants rXYZ, gXYZ, bXYZ as columns
    Vector3 white{};             // RGB: the media white point (wtpt)
    std::array<IccCurve, 3> trc; // RGB: rTRC, gTRC, bTRC; gray: kTRC first
    // A CMYK profile, in a build that reads them (core/cmyk_profile.hpp):
    // only its header and tag table are checked here, lcms2 reads the rest.
    bool cmyk = false;
    std::size_t size = 0; // the declared size: the bytes that belong to the profile
};

// invalid_profile for malformed bytes; unsupported_profile for valid profiles
// without RGB or gray matrix/TRC data, other than CMYK profiles in a build
// that reads them.
[[nodiscard]] Result<IccMatrixProfile> readIccMatrixProfile(std::span<const std::byte> profile);

} // namespace chromaroute::detail
