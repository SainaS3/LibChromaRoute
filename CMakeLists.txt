cmake_minimum_required(VERSION 3.28)

project(LibChromaRoute
  VERSION 0.0.1
  DESCRIPTION "Independent C++20 pixel-format and color-space conversion library"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(chromaroute_compiler_options)

option(CHROMAROUTE_BUILD_TESTS "Build LibChromaRoute tests" ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Core library (portable C++20; no platform UI or graphics dependencies)
# ---------------------------------------------------------------------------
add_library(chromaroute STATIC
  src/color_space.cpp
  src/converter.cpp
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

if(CHROMAROUTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
