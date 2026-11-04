#pragma once

// ICC profiles built on lookup tables, evaluated through Little CMS 2
// (decisions D1, A11, A12): CMYK profiles, and RGB and gray profiles that have
// no matrix/TRC form to read. A profile converts between its device values and
// linear sRGB (scRGB) with relative colorimetric intent and black point
// compensation, as babl does for CMYK. In a build without
// CHROMAROUTE_WITH_LCMS2 no profile opens.

#include <chromaroute/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace chromaroute::detail {

struct DeviceProfile;
struct DeviceLink;

struct DeviceProfileDeleter {
    void operator()(const DeviceProfile* profile) const noexcept;
};
using DeviceProfilePtr = std::unique_ptr<const DeviceProfile, DeviceProfileDeleter>;
using DeviceLinkPtr = std::shared_ptr<const DeviceLink>;

// The profile's data color space, and so its device channels: 1, 3 or 4.
enum class DeviceKind : std::uint8_t { gray, rgb, cmyk };

[[nodiscard]] constexpr std::size_t deviceChannels(DeviceKind kind) noexcept
{
    return kind == DeviceKind::gray ? 1 : kind == DeviceKind::rgb ? 3 : 4;
}

// Whether this build reads lookup-table profiles.
[[nodiscard]] bool deviceProfilesAvailable() noexcept;

// Opens a profile of `kind` whose header and tag table the reader has checked:
// invalid_profile when lcms2 cannot read it or build its transforms,
// unsupported_profile when it has no table into the connection space for
// relative colorimetric intent, or, for CMYK, none out of it. An RGB or gray
// profile without a table out of it opens without encoding (A12). Throws
// std::bad_alloc when lcms2 runs out of memory.
[[nodiscard]] Result<DeviceProfilePtr> openDeviceProfile(std::span<const std::byte> bytes, DeviceKind kind);

// The bytes the profile was opened from (profiles are interned by them).
[[nodiscard]] std::span<const std::byte> profileBytes(const DeviceProfile& profile) noexcept;
[[nodiscard]] DeviceKind profileKind(const DeviceProfile& profile) noexcept;
// Whether linear scRGB converts into the profile's device values.
[[nodiscard]] bool profileEncodes(const DeviceProfile& profile) noexcept;

// n pixels of the profile's device values (CMYK: 1 = full ink; RGB and gray:
// 0 to 1) to 3 linear scRGB values, and back (precondition: profileEncodes).
// A pixel with a NaN or infinite input gives NaN in every output.
void deviceToScrgb(const DeviceProfile& profile, const double* device, double* rgb, std::size_t n) noexcept;
void scrgbToDevice(const DeviceProfile& profile, const double* rgb, double* device, std::size_t n) noexcept;

// From one CMYK profile's inks to another's, through the connection space,
// with the same intent; nullptr when lcms2 cannot build it. Throws
// std::bad_alloc.
[[nodiscard]] DeviceLinkPtr linkCmykProfiles(const DeviceProfile& from, const DeviceProfile& to);
void cmykToCmyk(const DeviceLink& link, const double* in, double* out, std::size_t n) noexcept;

// Creates what every profile needs (lcms2's context and the scRGB profile),
// once; openDeviceProfile does it on first use. Throws std::bad_alloc.
void prepareDeviceProfiles();

// Test hooks. lcms2 allocates through the library's handler, which can make
// the n-th allocation from now fail (n = 0: none; returns what was left of the
// previous count, 0 once it failed) and counts the blocks allocated and not
// yet freed. They do nothing without lcms2.
long failLcmsAllocation(long n) noexcept;
[[nodiscard]] long liveLcmsAllocations() noexcept;

} // namespace chromaroute::detail
