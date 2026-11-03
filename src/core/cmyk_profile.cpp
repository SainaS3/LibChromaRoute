#include "core/cmyk_profile.hpp"

#if CHROMAROUTE_WITH_LCMS2
#include "engine/color_math.hpp"

#include <lcms2.h>
#include <lcms2_plugin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>
#endif

namespace chromaroute::detail {

#if CHROMAROUTE_WITH_LCMS2

struct CmykProfile {
    std::vector<std::byte> bytes;
    cmsHPROFILE profile = nullptr; // kept open for links to other profiles
    cmsHTRANSFORM to_rgb = nullptr;
    cmsHTRANSFORM from_rgb = nullptr;
};

struct CmykLink {
    cmsHTRANSFORM transform = nullptr;
    ~CmykLink()
    {
        if (transform != nullptr) {
            cmsDeleteTransform(transform);
        }
    }
};

namespace {

// lcms2 reports problems to a context-wide handler and allocates through a
// context-wide plugin. The handler keeps the last message per thread for
// error texts; the allocator records a failure per thread, so that an
// exhausted heap throws std::bad_alloc (decision A2) rather than reading as
// a bad profile.
thread_local char last_message[256];
thread_local bool out_of_memory = false;

void logError(cmsContext, cmsUInt32Number, const char* text)
{
    std::snprintf(last_message, sizeof last_message, "%s", text != nullptr ? text : "");
}

// The test hooks' state (failLcmsAllocation, liveLcmsAllocations).
std::atomic<long> live_allocations{0};
std::atomic<long> fail_countdown{0}; // > 0: the allocation that brings it to 0 fails

bool injectedFailure() noexcept
{
    return fail_countdown.load() > 0 && --fail_countdown == 0;
}

void* allocate(cmsContext, cmsUInt32Number size)
{
    void* p = injectedFailure() ? nullptr : std::malloc(size);
    if (p == nullptr) {
        out_of_memory = true;
        return nullptr;
    }
    ++live_allocations;
    return p;
}

void release(cmsContext, void* p)
{
    if (p != nullptr) {
        --live_allocations;
        std::free(p);
    }
}

void* reallocate(cmsContext, void* p, cmsUInt32Number size)
{
    void* q = injectedFailure() ? nullptr : std::realloc(p, size);
    if (q == nullptr) {
        out_of_memory = true; // the old block, if any, stays
        return nullptr;
    }
    if (p == nullptr) {
        ++live_allocations;
    }
    return q;
}

// Starts a call into lcms2: clears the per-thread state it reports through.
void beginCall() noexcept
{
    last_message[0] = '\0';
    out_of_memory = false;
}

// After a call: std::bad_alloc if lcms2 ran out of memory during it, also
// where lcms2 carried on (it leaves some allocations unchecked).
void throwIfOutOfMemory()
{
    if (out_of_memory) {
        throw std::bad_alloc();
    }
}

std::string lastMessage(const char* fallback)
{
    return last_message[0] != '\0' ? std::string(last_message) : std::string(fallback);
}

// lcms2's own mutexes crash on one whose allocation failed: a new profile
// keeps the null pointer, then locks and destroys it. These allocate through
// the handler above, like everything else, and treat a null mutex as nothing
// to lock. A null one only comes with a failed allocation, after which the
// call throws and the object is released, so no profile is kept without one.
void* createMutex(cmsContext ctx)
{
    void* p = allocate(ctx, sizeof(std::mutex));
    return p != nullptr ? new (p) std::mutex : nullptr;
}

void destroyMutex(cmsContext ctx, void* m)
{
    if (m != nullptr) {
        static_cast<std::mutex*>(m)->~mutex();
        release(ctx, m);
    }
}

cmsBool lockMutex(cmsContext, void* m)
{
    if (m == nullptr) {
        return TRUE;
    }
    try {
        static_cast<std::mutex*>(m)->lock();
    } catch (...) {
        return FALSE; // lcms2 reports the failure
    }
    return TRUE;
}

void unlockMutex(cmsContext, void* m)
{
    if (m != nullptr) {
        static_cast<std::mutex*>(m)->unlock();
    }
}

struct ContextDeleter {
    void operator()(cmsContext c) const noexcept { cmsDeleteContext(c); }
};
struct ProfileCloser {
    void operator()(void* p) const noexcept { cmsCloseProfile(p); }
};
struct CurveFreer {
    void operator()(cmsToneCurve* c) const noexcept { cmsFreeToneCurve(c); }
};
using ContextOwner = std::unique_ptr<std::remove_pointer_t<cmsContext>, ContextDeleter>;
using ProfileOwner = std::unique_ptr<void, ProfileCloser>;
using CurveOwner = std::unique_ptr<cmsToneCurve, CurveFreer>;

// What every profile needs: the library's lcms2 context, with the handlers
// above, and scRGB as an lcms2 profile, holding the library's own exact
// D50-adapted matrix for sRGB's primaries (N4) and linear curves, so lcms2
// meets the RGB side of CMYK (S5) with the same numbers as the engine. Both
// are made on first use and never destroyed (like the registry).
struct Lcms {
    cmsContext context;
    cmsHPROFILE scrgb;
};

// Until both exist each object has an owner, so a failed attempt
// (std::bad_alloc) releases everything it made, and the next use tries again.
Lcms makeLcms()
{
    static cmsPluginMutex mutexes{{cmsPluginMagicNumber, LCMS_VERSION, cmsPluginMutexSig, nullptr},
                                  createMutex, destroyMutex, lockMutex, unlockMutex};
    static cmsPluginMemHandler memory{{cmsPluginMagicNumber, LCMS_VERSION, cmsPluginMemHandlerSig, &mutexes.base},
                                      allocate, release, reallocate, nullptr, nullptr, nullptr};
    beginCall();
    ContextOwner ctx(cmsCreateContext(&memory, nullptr));
    throwIfOutOfMemory();
    if (ctx == nullptr) {
        throw std::bad_alloc();
    }
    cmsSetLogErrorHandlerTHR(ctx.get(), logError);
    ProfileOwner scrgb(cmsCreateProfilePlaceholder(ctx.get()));
    CurveOwner linear(cmsBuildGamma(ctx.get(), 1.0));
    throwIfOutOfMemory();
    if (scrgb == nullptr || linear == nullptr) {
        throw std::bad_alloc();
    }
    cmsSetProfileVersion(scrgb.get(), 4.3);
    cmsSetDeviceClass(scrgb.get(), cmsSigDisplayClass);
    cmsSetColorSpace(scrgb.get(), cmsSigRgbData);
    cmsSetPCS(scrgb.get(), cmsSigXYZData);
    const Matrix3 m = rgbToXyzD50(srgb_chromaticities);
    const cmsCIEXYZ white{icc_d50_white[0], icc_d50_white[1], icc_d50_white[2]};
    const cmsCIEXYZ red{m[0][0], m[1][0], m[2][0]};
    const cmsCIEXYZ green{m[0][1], m[1][1], m[2][1]};
    const cmsCIEXYZ blue{m[0][2], m[1][2], m[2][2]};
    const bool written = cmsWriteTag(scrgb.get(), cmsSigMediaWhitePointTag, &white) &&
                         cmsWriteTag(scrgb.get(), cmsSigRedColorantTag, &red) &&
                         cmsWriteTag(scrgb.get(), cmsSigGreenColorantTag, &green) &&
                         cmsWriteTag(scrgb.get(), cmsSigBlueColorantTag, &blue) &&
                         cmsWriteTag(scrgb.get(), cmsSigRedTRCTag, linear.get()) &&
                         cmsWriteTag(scrgb.get(), cmsSigGreenTRCTag, linear.get()) &&
                         cmsWriteTag(scrgb.get(), cmsSigBlueTRCTag, linear.get());
    throwIfOutOfMemory();
    if (!written) {
        throw std::bad_alloc(); // writing in-memory tags fails only for memory
    }
    return Lcms{ctx.release(), scrgb.release()}; // the profile holds copies of the curve
}

const Lcms& lcms()
{
    static const Lcms instance = makeLcms();
    return instance;
}

// Relative colorimetric with black point compensation, as babl (A11).
constexpr cmsUInt32Number transform_flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE;

// Whether the profile has a table lcms2 uses for relative colorimetric intent
// in one direction: the float D2B1 or B2D1, else the 16-bit relative table,
// else the perceptual one, as lcms2 falls back (cmsio1.c, _cmsReadInputLUT and
// _cmsReadOutputLUT).
bool hasRelativeTable(cmsHPROFILE p, bool to_pcs)
{
    if (to_pcs) {
        return cmsIsTag(p, cmsSigDToB1Tag) || cmsIsTag(p, cmsSigAToB1Tag) || cmsIsTag(p, cmsSigAToB0Tag);
    }
    return cmsIsTag(p, cmsSigBToD1Tag) || cmsIsTag(p, cmsSigBToA1Tag) || cmsIsTag(p, cmsSigBToA0Tag);
}

// lcms2 dereferences null transforms, so only what was built is released.
void destroy(const CmykProfile& p) noexcept
{
    if (p.to_rgb != nullptr) {
        cmsDeleteTransform(p.to_rgb);
    }
    if (p.from_rgb != nullptr) {
        cmsDeleteTransform(p.from_rgb);
    }
    if (p.profile != nullptr) {
        cmsCloseProfile(p.profile);
    }
}

bool finitePixel(const double* v, std::size_t count) noexcept
{
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

// Runs a transform in blocks, scaling inks between 0..1 here and 0..100 in
// lcms2's CMYK double format. Pixels with a non-finite input go to lcms2 as 0
// and come back as NaN in every output.
void run(cmsHTRANSFORM t, const double* in, std::size_t in_count, double in_scale, double* out, std::size_t out_count,
         double out_scale, std::size_t n) noexcept
{
    constexpr std::size_t block = 128;
    double scaled[block * 4];
    bool finite[block];
    for (std::size_t done = 0; done < n;) {
        const std::size_t pixels = n - done < block ? n - done : block;
        for (std::size_t p = 0; p < pixels; ++p) {
            const double* v = in + (done + p) * in_count;
            finite[p] = finitePixel(v, in_count);
            for (std::size_t i = 0; i < in_count; ++i) {
                scaled[p * in_count + i] = finite[p] ? v[i] * in_scale : 0.0;
            }
        }
        double* o = out + done * out_count;
        cmsDoTransform(t, scaled, o, static_cast<cmsUInt32Number>(pixels));
        for (std::size_t p = 0; p < pixels; ++p) {
            for (std::size_t i = 0; i < out_count; ++i) {
                o[p * out_count + i] = finite[p] ? o[p * out_count + i] * out_scale : std::nan("");
            }
        }
        done += pixels;
    }
}

} // namespace

void CmykProfileDeleter::operator()(const CmykProfile* profile) const noexcept
{
    if (profile != nullptr) {
        destroy(*profile);
        delete profile;
    }
}

bool cmykProfilesAvailable() noexcept
{
    return true;
}

void prepareCmykProfiles()
{
    (void)lcms();
}

long failLcmsAllocation(long n) noexcept
{
    return fail_countdown.exchange(n);
}

long liveLcmsAllocations() noexcept
{
    return live_allocations.load();
}

Result<CmykProfilePtr> openCmykProfile(std::span<const std::byte> bytes)
{
    const Lcms& l = lcms();
    // Owned until complete: on any early return or throw it releases what
    // lcms2 made for it.
    std::unique_ptr<CmykProfile, CmykProfileDeleter> p(new CmykProfile{std::vector<std::byte>(bytes.begin(), bytes.end())});
    beginCall();
    p->profile = cmsOpenProfileFromMemTHR(l.context, p->bytes.data(), static_cast<cmsUInt32Number>(p->bytes.size()));
    throwIfOutOfMemory();
    if (p->profile == nullptr) {
        return Error{ErrorCode::invalid_profile, "invalid ICC profile: " + lastMessage("lcms2 cannot read it")};
    }
    if (!hasRelativeTable(p->profile, true) || !hasRelativeTable(p->profile, false)) {
        return Error{ErrorCode::unsupported_profile,
                     "unsupported ICC profile: a CMYK profile needs tables both ways (A2B0 or A2B1, and B2A0 or B2A1)"};
    }
    p->to_rgb = cmsCreateTransformTHR(l.context, p->profile, TYPE_CMYK_DBL, l.scrgb, TYPE_RGB_DBL,
                                      INTENT_RELATIVE_COLORIMETRIC, transform_flags);
    if (p->to_rgb != nullptr) {
        p->from_rgb = cmsCreateTransformTHR(l.context, l.scrgb, TYPE_RGB_DBL, p->profile, TYPE_CMYK_DBL,
                                            INTENT_RELATIVE_COLORIMETRIC, transform_flags);
    }
    throwIfOutOfMemory();
    if (p->to_rgb == nullptr || p->from_rgb == nullptr) {
        return Error{ErrorCode::invalid_profile,
                     "invalid ICC profile: " + lastMessage("lcms2 cannot build its CMYK transforms")};
    }
    return CmykProfilePtr(p.release());
}

std::span<const std::byte> profileBytes(const CmykProfile& profile) noexcept
{
    return profile.bytes;
}

void cmykToScrgb(const CmykProfile& profile, const double* inks, double* rgb, std::size_t n) noexcept
{
    run(profile.to_rgb, inks, 4, 100.0, rgb, 3, 1.0, n);
}

void scrgbToCmyk(const CmykProfile& profile, const double* rgb, double* inks, std::size_t n) noexcept
{
    run(profile.from_rgb, rgb, 3, 1.0, inks, 4, 0.01, n);
}

CmykLinkPtr linkCmykProfiles(const CmykProfile& from, const CmykProfile& to)
{
    const Lcms& l = lcms();
    auto link = std::make_shared<CmykLink>(); // releases the transform on a throw
    beginCall();
    link->transform = cmsCreateTransformTHR(l.context, from.profile, TYPE_CMYK_DBL, to.profile, TYPE_CMYK_DBL,
                                            INTENT_RELATIVE_COLORIMETRIC, transform_flags);
    throwIfOutOfMemory();
    if (link->transform == nullptr) {
        return nullptr;
    }
    return link;
}

void cmykToCmyk(const CmykLink& link, const double* in, double* out, std::size_t n) noexcept
{
    run(link.transform, in, 4, 100.0, out, 4, 0.01, n);
}

#else // Without lcms2 no CMYK profile opens, so the evaluators are never reached.

struct CmykProfile {};
struct CmykLink {};

void CmykProfileDeleter::operator()(const CmykProfile* profile) const noexcept
{
    delete profile;
}

bool cmykProfilesAvailable() noexcept
{
    return false;
}

void prepareCmykProfiles() {}

long failLcmsAllocation(long) noexcept
{
    return 0;
}

long liveLcmsAllocations() noexcept
{
    return 0;
}

Result<CmykProfilePtr> openCmykProfile(std::span<const std::byte>)
{
    return Error{ErrorCode::unsupported_profile,
                 "unsupported ICC profile: CMYK profiles need a build with lcms2 (CHROMAROUTE_WITH_LCMS2)"};
}

std::span<const std::byte> profileBytes(const CmykProfile&) noexcept
{
    return {};
}

void cmykToScrgb(const CmykProfile&, const double*, double*, std::size_t) noexcept {}
void scrgbToCmyk(const CmykProfile&, const double*, double*, std::size_t) noexcept {}

CmykLinkPtr linkCmykProfiles(const CmykProfile&, const CmykProfile&)
{
    return nullptr;
}

void cmykToCmyk(const CmykLink&, const double*, double*, std::size_t) noexcept {}

#endif

} // namespace chromaroute::detail
