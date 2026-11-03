# Little CMS 2, the optional back end for CMYK ICC profiles (decisions D1, P5,
# A11): one pinned release, checked by SHA-256, built from source in this build
# as a static library with lcms2's own CMake. Only the library is built: no
# shared library, tools, tests or (GPL) plugins.
#
# The release is patched on extraction with the fixes in
# cmake/patches/lcms2-2.20 (decision P5): failure paths that leak or crash when
# an allocation fails. A patch that does not apply stops the configuration.

include(FetchContent)

set(LCMS2_BUILD_SHARED OFF)
set(LCMS2_BUILD_STATIC ON)
set(LCMS2_BUILD_TOOLS OFF)
set(LCMS2_BUILD_TESTS OFF)
set(LCMS2_WITH_FASTFLOAT OFF)
set(LCMS2_WITH_THREADED_PLUGIN OFF)
# The UWP presets try-compile static libraries (CMAKE_TRY_COMPILE_TARGET_TYPE),
# which are never linked, so lcms2's check for gmtime_r passes without the
# function existing. MSVC's CRT has no gmtime_r; defined here, the check is
# skipped and lcms2 uses gmtime_s, which it detects by compiling.
if(MSVC)
  set(LCMS2_HAVE_GMTIME_R 0)
endif()

set(chromaroute_lcms2_patches "${CMAKE_CURRENT_LIST_DIR}/patches/lcms2-2.20")
# The source tree is named after the patches' digest: a changed set of patches
# extracts the release into a new tree and patches it there, never re-patching
# a tree that the old set patched.
file(GLOB patch_files "${chromaroute_lcms2_patches}/*.patch")
list(SORT patch_files)
set(patch_digests "")
foreach(patch_file IN LISTS patch_files)
  file(SHA256 "${patch_file}" digest)
  string(APPEND patch_digests "${digest}")
endforeach()
string(SHA256 patch_digest "${patch_digests}")
string(SUBSTRING "${patch_digest}" 0 12 patch_tag)

FetchContent_Declare(lcms2
  URL https://github.com/mm2/Little-CMS/releases/download/lcms2.20/lcms2-2.20.tar.gz
  URL_HASH SHA256=18ff5504fa9cc8a2b1f3591180d90ead16c8a2ea78e530523b46eab70e1ca6b9
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/lcms2-src-${patch_tag}"
  PATCH_COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=<SOURCE_DIR> "-DPATCH_DIR=${chromaroute_lcms2_patches}"
                -P "${CMAKE_CURRENT_LIST_DIR}/chromaroute_apply_patches.cmake"
  EXCLUDE_FROM_ALL
  SYSTEM)
FetchContent_MakeAvailable(lcms2)
