# Applies the patch files in PATCH_DIR, in name order, to a freshly extracted
# third-party source tree. Run as a FetchContent PATCH_COMMAND:
#   cmake -DSOURCE_DIR=<SOURCE_DIR> -DPATCH_DIR=<dir> -P chromaroute_apply_patches.cmake
# All or nothing: git apply applies each patch whole or not at all, and if one
# does not apply exactly, those before it are reversed. The configuration then
# stops, so the build never continues with an unpatched or partly patched
# dependency.

foreach(var SOURCE_DIR PATCH_DIR)
  if(NOT ${var})
    message(FATAL_ERROR "chromaroute_apply_patches: ${var} is not set")
  endif()
endforeach()
find_package(Git QUIET)
if(NOT GIT_FOUND)
  message(FATAL_ERROR "chromaroute_apply_patches: git is required to apply the patches in ${PATCH_DIR}")
endif()

file(GLOB patches "${PATCH_DIR}/*.patch")
list(SORT patches)
if(NOT patches)
  message(FATAL_ERROR "chromaroute_apply_patches: no patches in ${PATCH_DIR}")
endif()

# The source tree may sit inside another git work tree (a build directory in a
# checkout); stop git from finding it, so the patch paths stay relative to the
# source tree and nothing is read from the enclosing repository.
get_filename_component(parent "${SOURCE_DIR}" DIRECTORY)
set(ENV{GIT_CEILING_DIRECTORIES} "${parent}")

set(applied "")
foreach(patch IN LISTS patches)
  get_filename_component(name "${patch}" NAME)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
  if(NOT result EQUAL 0)
    list(REVERSE applied)
    foreach(done IN LISTS applied)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse "${done}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE undone)
      if(NOT undone EQUAL 0)
        message(FATAL_ERROR "chromaroute_apply_patches: ${name} does not apply, and ${done} could not be "
                            "reversed; delete ${SOURCE_DIR} before configuring again:\n${output}")
      endif()
    endforeach()
    message(FATAL_ERROR "chromaroute_apply_patches: ${name} does not apply; nothing in ${PATCH_DIR} "
                        "is applied:\n${output}")
  endif()
  list(APPEND applied "${patch}")
  message(STATUS "Applied ${name}")
endforeach()
