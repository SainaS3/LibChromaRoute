# Patches to Little CMS 2.20

Applied in name order to the pinned `lcms2-2.20.tar.gz` on extraction, by
`cmake/chromaroute_apply_patches.cmake` (decision P5). Each fixes an
allocation-failure path in lcms2 itself. Each is tested by
`tests/host/lcms2_patch_tests.c`, and each is drafted for upstream in
`docs/upstream/lcms2-2.20-allocation-failures.md`.

| Patch | Fixes |
| --- | --- |
| `0001-free-reversed-curves-on-failure.patch` | `BuildRGBOutputMatrixShaper` (`src/cmsio1.c`) leaks the reversed curves when one reversal fails |
| `0002-check-matrix-duplication.patch` | `MatrixElemDup` (`src/cmslut.c`) leaves its copies of the matrix and the offset unchecked |

## Provenance

The changes were written for LibChromaRoute. Each patch file also holds lines of
lcms2 2.20's source: the unchanged lines around each change, as context, and the
lines it changes. The files are therefore derived from lcms2, and those lines
remain under lcms2's MIT license; its copyright and permission notice is in
[`LICENSE-lcms2`](LICENSE-lcms2), copied unchanged from the release. Which
license LibChromaRoute's own changes carry is decided with the project license.
