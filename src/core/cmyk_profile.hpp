#pragma once

// CMYK ICC profiles, evaluated through Little CMS 2 (decisions D1, A11). A
// profile converts between its inks and linear sRGB (scRGB, the RGB side of
// CMYK, S5) with relative colorimetric intent and black point compensation,
// as babl does. In a build without CHROMAROUTE_WITH_LCMS2 no profile opens.

#include <chromaroute/result.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace chromaroute::detail {

struct CmykProfile;
struct CmykLink;

struct CmykProfileDeleter {
    void operator()(const CmykProfile* profile) const noexcept;
};
using CmykProfilePtr = std::unique_ptr<const CmykProfile, CmykProfileDeleter>;
using CmykLinkPtr = std::shared_ptr<const CmykLink>;

// Whether this build reads CMYK profiles.
[[nodiscard]] bool cmykProfilesAvailable() noexcept;

// Opens a CMYK profile whose header and tag table the reader has checked:
// invalid_profile when lcms2 cannot read it or build its transforms,
// unsupported_profile when it does not convert both ways with relative
// colorimetric intent. Throws std::bad_alloc when lcms2 runs out of memory.
[[nodiscard]] Result<CmykProfilePtr> openCmykProfile(std::span<const std::byte> bytes);

// The bytes the profile was opened from (profiles are interned by them).
[[nodiscard]] std::span<const std::byte> profileBytes(const CmykProfile& profile) noexcept;

// n pixels of 4 inks (1 = full ink) to 3 linear scRGB values, and back. A
// pixel with a NaN or infinite input gives NaN in every output.
void cmykToScrgb(const CmykProfile& profile, const double* inks, double* rgb, std::size_t n) noexcept;
void scrgbToCmyk(const CmykProfile& profile, const double* rgb, double* inks, std::size_t n) noexcept;

// From one profile's inks to another's, through the connection space, with
// the same intent; nullptr when lcms2 cannot build it. Throws std::bad_alloc.
[[nodiscard]] CmykLinkPtr linkCmykProfiles(const CmykProfile& from, const CmykProfile& to);
void cmykToCmyk(const CmykLink& link, const double* in, double* out, std::size_t n) noexcept;

// Creates what every profile needs (lcms2's context and the scRGB profile),
// once; openCmykProfile does it on first use. Throws std::bad_alloc.
void prepareCmykProfiles();

// Test hooks. lcms2 allocates through the library's handler, which can make
// the n-th allocation from now fail (n = 0: none; returns what was left of the
// previous count, 0 once it failed) and counts the blocks allocated and not
// yet freed. They do nothing without lcms2.
long failLcmsAllocation(long n) noexcept;
[[nodiscard]] long liveLcmsAllocations() noexcept;

} // namespace chromaroute::detail
