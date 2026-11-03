/* Allocation-failure checks for the patches LibChromaRoute applies to Little
 * CMS 2.20 (cmake/patches/lcms2-2.20). Only lcms2's public and plugin API is
 * used, so this file is also the standalone reproducer for the upstream
 * reports: against an unpatched 2.20 each mode fails or crashes.
 *
 *   lcms2_patch_tests curves   0001: a failed output matrix-shaper leaks no
 *                              reversed curve
 *   lcms2_patch_tests matrix   0002: a failed copy of a matrix stage, of its
 *                              matrix or of its offset, is no copy
 *
 * A counting allocator fails the n-th allocation of the call under test, for
 * n = 1, 2, ..., until the call needs fewer. After every attempt no block it
 * made may stay allocated, and whatever it returns must work as one made with
 * no failure. */

#include <lcms2_plugin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long fail_at;     /* the allocation to fail, counted from 1; 0 fails none */
static long allocations; /* allocations since fail_at was set */
static long live;        /* blocks allocated and not yet freed */
static int failures;

static void* countedMalloc(cmsContext id, cmsUInt32Number size)
{
    void* p;
    (void)id;
    if (fail_at != 0 && ++allocations == fail_at) {
        return NULL;
    }
    p = malloc(size);
    if (p != NULL) {
        ++live;
    }
    return p;
}

static void countedFree(cmsContext id, void* p)
{
    (void)id;
    if (p != NULL) {
        --live;
        free(p);
    }
}

static void* countedRealloc(cmsContext id, void* p, cmsUInt32Number size)
{
    void* q;
    (void)id;
    if (fail_at != 0 && ++allocations == fail_at) {
        return NULL;
    }
    q = realloc(p, size);
    if (q != NULL && p == NULL) {
        ++live;
    }
    return q;
}

/* lcms2's default mutexes crash on one whose allocation failed (a new profile
 * keeps the null mutex, then locks and destroys it), a third issue reported
 * on its own. This single-threaded test needs no locking: these allocate a
 * token through the counted allocator, so its failure is exercised too, and
 * treat a null one as nothing to lock. */
static void* createMutex(cmsContext id)
{
    return countedMalloc(id, 1);
}

static void destroyMutex(cmsContext id, void* m)
{
    countedFree(id, m);
}

static cmsBool lockMutex(cmsContext id, void* m)
{
    (void)id;
    (void)m;
    return TRUE;
}

static void unlockMutex(cmsContext id, void* m)
{
    (void)id;
    (void)m;
}

static void check(int ok, const char* what, long n)
{
    if (!ok) {
        ++failures;
        if (failures <= 10) {
            printf("  FAILED (allocation %ld failing): %s\n", n, what);
        }
    }
}

/* Starts an attempt with the n-th allocation failing. */
static void failAllocation(long n)
{
    fail_at = n;
    allocations = 0;
}

/* Ends the attempt; returns whether it reached the failing allocation. */
static int reachedFailure(void)
{
    const int reached = fail_at != 0 && allocations >= fail_at;
    fail_at = 0;
    return reached;
}

/* 0001: an XYZ-to-sRGB transform builds sRGB's output matrix-shaper
 * (BuildRGBOutputMatrixShaper), which reverses its three tone curves. */
static void curvesTest(cmsContext ctx)
{
    const cmsFloat64Number xyz_in[3] = {0.3, 0.4, 0.2};
    cmsFloat64Number expected[3] = {0, 0, 0};
    long n;
    int injected = 0;
    for (n = 0; n < 100000; ++n) {
        const long before = live;
        cmsHPROFILE xyz = cmsCreateXYZProfileTHR(ctx);
        cmsHPROFILE srgb = cmsCreate_sRGBProfileTHR(ctx);
        cmsHTRANSFORM t;
        int reached;
        if (xyz == NULL || srgb == NULL) {
            check(0, "the profiles are made", n);
            return;
        }
        failAllocation(n);
        t = cmsCreateTransformTHR(ctx, xyz, TYPE_XYZ_DBL, srgb, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                  cmsFLAGS_NOCACHE);
        reached = reachedFailure();
        if (t != NULL) {
            cmsFloat64Number rgb[3] = {-1, -1, -1};
            cmsDoTransform(t, xyz_in, rgb, 1);
            if (n == 0) {
                memcpy(expected, rgb, sizeof rgb);
            } else if (!reached) {
                check(memcmp(rgb, expected, sizeof rgb) == 0, "the transform converts as one made with no failure", n);
            }
            cmsDeleteTransform(t);
        }
        cmsCloseProfile(xyz);
        cmsCloseProfile(srgb);
        check(live == before, "an attempt leaves no block allocated", n);
        if (n != 0 && !reached) {
            check(t != NULL, "the transform is made once no allocation fails", n);
            break;
        }
        injected += n != 0;
    }
    printf("  curves: %d failed allocations injected\n", injected);
    check(injected >= 2, "the transform failed at every allocation, then was made", n);
}

/* 0002: copying a pipeline copies its matrix stage (MatrixElemDup), the
 * matrix and the offset each in an allocation of their own. */
static void matrixTest(cmsContext ctx)
{
    const cmsFloat64Number matrix[9] = {0.5, 0.25, 0.125, 0.0625, 0.5, 0.25, 0.125, 0.0625, 0.5};
    const cmsFloat64Number offset[3] = {0.125, 0.25, 0.375};
    const cmsFloat32Number in[3] = {0.25f, 0.5f, 0.75f};
    cmsFloat32Number expected[3] = {0, 0, 0};
    cmsPipeline* lut = cmsPipelineAlloc(ctx, 3, 3);
    cmsStage* stage = cmsStageAllocMatrix(ctx, 3, 3, matrix, offset);
    long n;
    int injected = 0;
    if (lut == NULL || stage == NULL || !cmsPipelineInsertStage(lut, cmsAT_END, stage)) {
        check(0, "the pipeline is made", 0);
        return;
    }
    cmsPipelineEvalFloat(in, expected, lut);
    for (n = 1; n < 100000; ++n) {
        const long before = live;
        cmsPipeline* copy;
        int reached;
        failAllocation(n);
        copy = cmsPipelineDup(lut);
        reached = reachedFailure();
        if (copy != NULL) {
            cmsFloat32Number out[3] = {-1, -1, -1};
            cmsPipelineEvalFloat(in, out, copy); /* unpatched: crashes on a missing matrix */
            check(memcmp(out, expected, sizeof out) == 0, "a copy evaluates as the original", n);
            cmsPipelineFree(copy);
        }
        check(live == before, "an attempt leaves no block allocated", n);
        if (!reached) {
            check(copy != NULL, "the copy is made once no allocation fails", n);
            break;
        }
        check(copy == NULL, "a failed allocation makes no copy", n);
        ++injected;
    }
    printf("  matrix: %d failed allocations injected\n", injected);
    check(injected >= 3, "the copy failed at every allocation, then was made", n);
    cmsPipelineFree(lut);
}

int main(int argc, char** argv)
{
    static cmsPluginMutex mutexes = {{cmsPluginMagicNumber, LCMS_VERSION, cmsPluginMutexSig, NULL},
                                     createMutex, destroyMutex, lockMutex, unlockMutex};
    static cmsPluginMemHandler memory = {{cmsPluginMagicNumber, LCMS_VERSION, cmsPluginMemHandlerSig, &mutexes.base},
                                         countedMalloc, countedFree, countedRealloc, NULL, NULL, NULL};
    cmsContext ctx;
    const char* mode = argc == 2 ? argv[1] : "";
    if (strcmp(mode, "curves") != 0 && strcmp(mode, "matrix") != 0) {
        fprintf(stderr, "usage: lcms2_patch_tests curves|matrix\n");
        return 2;
    }
    ctx = cmsCreateContext(&memory, NULL);
    if (ctx == NULL) {
        printf("%s: the context is not made\n", mode);
        return 1;
    }
    if (strcmp(mode, "curves") == 0) {
        curvesTest(ctx);
    } else {
        matrixTest(ctx);
    }
    cmsDeleteContext(ctx);
    check(live == 0, "deleting the context frees every block", 0);
    printf("%s: %s\n", mode, failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
