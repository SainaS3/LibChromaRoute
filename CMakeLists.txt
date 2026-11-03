cmake_minimum_required(VERSION 3.28)

project(LibChromaRoute
  VERSION 0.0.1
  DESCRIPTION "Independent C++20 pixel-format and color-space conversion library"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(chromaroute_compiler_options)

option(CHROMAROUTE_BUILD_TESTS "Build LibChromaRoute tests" ${PROJECT_IS_TOP_LEVEL})
# Off by default: without it the core has no dependency, and CMYK profiles
# return an unsupported-profile error (decision P5). On, the first configure
# downloads the pinned lcms2 release.
option(CHROMAROUTE_WITH_LCMS2 "Read CMYK ICC profiles through Little CMS 2 (fetched, pinned)" OFF)

# ---------------------------------------------------------------------------
# Core library (portable C++20; no platform UI or graphics dependencies)
# ---------------------------------------------------------------------------
add_library(chromaroute STATIC
  src/color_space.cpp
  src/converter.cpp
  src/core/cmyk_profile.cpp
  src/core/icc_profile.cpp
  src/core/registry.cpp
  src/engine/conversion_plan.cpp
  src/pixel_format.cpp
  src/version.cpp)
add_library(ChromaRoute::chromaroute ALIAS chromaroute)

target_compile_features(chromaroute PUBLIC cxx_std_20)
target_include_directories(chromaroute
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(chromaroute
  PRIVATE
    CHROMAROUTE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CHROMAROUTE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CHROMAROUTE_VERSION_PATCH=${PROJECT_VERSION_PATCH})
chromaroute_apply_compiler_options(chromaroute)

if(CHROMAROUTE_WITH_LCMS2)
  enable_language(C) # lcms2, and the tests of its patches, are C
  include(chromaroute_lcms2)
  target_link_libraries(chromaroute PRIVATE lcms2)
  target_compile_definitions(chromaroute PRIVATE CHROMAROUTE_WITH_LCMS2=1)
endif()

if(CHROMAROUTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
