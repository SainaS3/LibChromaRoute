# Test sources shared by the host runner and the packaged UWP runner.
set(CHROMAROUTE_TEST_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/support/test_harness.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/cie_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/cmyk_profile_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/cmyk_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/color_math_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/color_semantics_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/color_space_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/concurrency_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/converter_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/half_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/hue_model_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/icc_profile_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/lut_profile_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/model_conversion_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/numeric_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/oklab_ycbcr_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/pipeline_property_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/pixel_format_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/srgb_r1_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/transfer_curve_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/version_tests.cpp)

function(chromaroute_add_test_runner target)
  target_sources(${target} PRIVATE ${CHROMAROUTE_TEST_SOURCES})
  # Unit tests may exercise internals, so the private source tree is visible.
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${target} PRIVATE ChromaRoute::chromaroute)
  target_compile_definitions(${target} PRIVATE
    CHROMAROUTE_TEST_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CHROMAROUTE_TEST_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CHROMAROUTE_TEST_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH})
  if(CHROMAROUTE_WITH_LCMS2)
    target_compile_definitions(${target} PRIVATE CHROMAROUTE_TEST_WITH_LCMS2=1)
  endif()
  chromaroute_apply_compiler_options(${target})
endfunction()

if(WINDOWS_STORE)
  # UWP executables only run when packaged and registered, so the UWP build
  # tree tests through a packaged app instead of a plain console runner.
  add_subdirectory(platform/uwp)
else()
  add_executable(chromaroute_tests host/main.cpp)
  chromaroute_add_test_runner(chromaroute_tests)
  add_test(NAME chromaroute_tests COMMAND chromaroute_tests)

  # Out-of-memory behavior. The test replaces the global operator new, so it
  # is its own executable, and each mode runs in a fresh process.
  add_executable(chromaroute_allocation_failure_tests host/allocation_failure_tests.cpp)
  target_link_libraries(chromaroute_allocation_failure_tests PRIVATE ChromaRoute::chromaroute)
  chromaroute_apply_compiler_options(chromaroute_allocation_failure_tests)
  foreach(mode first-touch plan-cache space-variant concurrent-first-touch)
    add_test(NAME allocation_failure_${mode} COMMAND chromaroute_allocation_failure_tests ${mode})
  endforeach()
  if(CHROMAROUTE_WITH_LCMS2)
    # lcms2 allocates through the library's handler, whose test hooks are
    # internal; the baseline is measured in a process of its own.
    target_include_directories(chromaroute_allocation_failure_tests PRIVATE
      ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(chromaroute_allocation_failure_tests PRIVATE CHROMAROUTE_TEST_WITH_LCMS2=1)
    set(lcms2_baseline ${CMAKE_CURRENT_BINARY_DIR}/lcms2_baseline.txt)
    add_test(NAME allocation_failure_lcms2-baseline
      COMMAND chromaroute_allocation_failure_tests lcms2-baseline ${lcms2_baseline})
    set_tests_properties(allocation_failure_lcms2-baseline PROPERTIES FIXTURES_SETUP lcms2_baseline)
    add_test(NAME allocation_failure_lcms2-first-touch
      COMMAND chromaroute_allocation_failure_tests lcms2-first-touch ${lcms2_baseline})
    set_tests_properties(allocation_failure_lcms2-first-touch PROPERTIES FIXTURES_REQUIRED lcms2_baseline)
    add_test(NAME allocation_failure_lcms2-profile COMMAND chromaroute_allocation_failure_tests lcms2-profile)

    # The patches applied to lcms2, each checked through lcms2's own API at a
    # failure path the library's flows never reach; also the upstream reproducer.
    add_executable(chromaroute_lcms2_patch_tests host/lcms2_patch_tests.c)
    target_link_libraries(chromaroute_lcms2_patch_tests PRIVATE lcms2)
    if(MSVC)
      target_compile_options(chromaroute_lcms2_patch_tests PRIVATE /W4 /WX /utf-8)
    else()
      target_compile_options(chromaroute_lcms2_patch_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    foreach(mode curves matrix)
      add_test(NAME lcms2_patch_${mode} COMMAND chromaroute_lcms2_patch_tests ${mode})
    endforeach()
  endif()

  # Every binary32 input of the R1 encoder; minutes in Debug, so Release only.
  add_executable(chromaroute_exhaustive_tests exhaustive/r1_encode_exhaustive.cpp)
  target_include_directories(chromaroute_exhaustive_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(chromaroute_exhaustive_tests PRIVATE ChromaRoute::chromaroute)
  chromaroute_apply_compiler_options(chromaroute_exhaustive_tests)
  add_test(NAME exhaustive_r1_encode COMMAND chromaroute_exhaustive_tests CONFIGURATIONS Release)
  set_tests_properties(exhaustive_r1_encode PROPERTIES LABELS "exhaustive" TIMEOUT 1800)

  # Every R'G'B' u8 color to CIE Lab u8 and LCH(ab) float, and every binary32
  # input of the range encoders (an internal function, so the private source
  # tree is visible); Release only, like the encoder above.
  add_executable(chromaroute_cie_exhaustive_tests exhaustive/cie_lab_exhaustive.cpp)
  target_include_directories(chromaroute_cie_exhaustive_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(chromaroute_cie_exhaustive_tests PRIVATE ChromaRoute::chromaroute)
  chromaroute_apply_compiler_options(chromaroute_cie_exhaustive_tests)
  add_test(NAME exhaustive_cie COMMAND chromaroute_cie_exhaustive_tests CONFIGURATIONS Release)
  set_tests_properties(exhaustive_cie PROPERTIES LABELS "exhaustive" TIMEOUT 1800)

  # Every R'G'B' u8 color to Y'CbCr u8, and every binary32 input of the
  # limited-range encoders; Release only, like the tests above.
  add_executable(chromaroute_ycbcr_exhaustive_tests exhaustive/ycbcr_exhaustive.cpp)
  target_include_directories(chromaroute_ycbcr_exhaustive_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(chromaroute_ycbcr_exhaustive_tests PRIVATE ChromaRoute::chromaroute)
  chromaroute_apply_compiler_options(chromaroute_ycbcr_exhaustive_tests)
  add_test(NAME exhaustive_ycbcr COMMAND chromaroute_ycbcr_exhaustive_tests CONFIGURATIONS Release)
  set_tests_properties(exhaustive_ycbcr PROPERTIES LABELS "exhaustive" TIMEOUT 1800)

  # Every R'G'B' u8 color to HSL, HSV and HCY float, and round trips through
  # each in float and double; Release only, like the tests above.
  add_executable(chromaroute_hue_exhaustive_tests exhaustive/hue_exhaustive.cpp)
  target_link_libraries(chromaroute_hue_exhaustive_tests PRIVATE ChromaRoute::chromaroute)
  chromaroute_apply_compiler_options(chromaroute_hue_exhaustive_tests)
  add_test(NAME exhaustive_hue COMMAND chromaroute_hue_exhaustive_tests CONFIGURATIONS Release)
  set_tests_properties(exhaustive_hue PROPERTIES LABELS "exhaustive" TIMEOUT 1800)

  # Every linear u8 color to CMYK u8 and cmyk u8, and round trips through the
  # CMYK models; Release only, like the tests above.
  add_executable(chromaroute_cmyk_exhaustive_tests exhaustive/cmyk_exhaustive.cpp)
  target_link_libraries(chromaroute_cmyk_exhaustive_tests PRIVATE ChromaRoute::chromaroute)
  chromaroute_apply_compiler_options(chromaroute_cmyk_exhaustive_tests)
  add_test(NAME exhaustive_cmyk COMMAND chromaroute_cmyk_exhaustive_tests CONFIGURATIONS Release)
  set_tests_properties(exhaustive_cmyk PROPERTIES LABELS "exhaustive" TIMEOUT 1800)
endif()
