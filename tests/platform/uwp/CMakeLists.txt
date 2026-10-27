# Packaged UWP console app that runs the shared test sources inside an
# AppContainer, plus a CTest entry that registers, runs and unregisters it.

set(CHROMAROUTE_UWP_PACKAGE_NAME "LibChromaRoute.Tests")
set(CHROMAROUTE_UWP_PACKAGE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
set(CHROMAROUTE_UWP_EXECUTABLE "chromaroute_uwp_tests.exe")
set(CHROMAROUTE_UWP_ALIAS "chromaroute-uwp-tests.exe")
set(CHROMAROUTE_UWP_MIN_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
set(CHROMAROUTE_UWP_MAX_VERSION_TESTED "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
configure_file(Package.appxmanifest.in "${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest" @ONLY)

set(assets
  Assets/StoreLogo.png
  Assets/Square150x150Logo.png
  Assets/Square44x44Logo.png)

add_executable(chromaroute_uwp_tests
  main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/Package.appxmanifest"
  ${assets})
set_property(SOURCE ${assets} PROPERTY VS_DEPLOYMENT_CONTENT 1)
set_property(SOURCE ${assets} PROPERTY VS_DEPLOYMENT_LOCATION "Assets")
# The package is only registered from a loose layout in Developer Mode, which
# needs no signature; skipping signing avoids handling certificates at all.
# MSBuild's package file names differ per configuration (Release has no
# suffix), so each configuration gets its own output directory instead.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/AppPackages" uwp_package_root)
set_target_properties(chromaroute_uwp_tests PROPERTIES
  VS_GLOBAL_AppxPackageSigningEnabled "false"
  VS_GLOBAL_AppxPackageDir "${uwp_package_root}\\$(Configuration)\\")
# App projects link with /OPT:ICF in every configuration, which conflicts with
# CMake's Debug /INCREMENTAL (LNK4075).
target_link_options(chromaroute_uwp_tests PRIVATE "$<$<CONFIG:Debug>:/INCREMENTAL:NO>")
chromaroute_add_test_runner(chromaroute_uwp_tests)

find_program(CHROMAROUTE_WINDOWS_POWERSHELL NAMES powershell REQUIRED)
find_program(CHROMAROUTE_MAKEAPPX NAMES makeappx
  HINTS "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  REGISTRY_VIEW BOTH
  REQUIRED)

add_test(NAME uwp_runtime_tests
  COMMAND "${CHROMAROUTE_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/run_uwp_test_package.ps1"
    -AppPackagesDir "${CMAKE_CURRENT_BINARY_DIR}/AppPackages/$<CONFIG>"
    -Configuration "$<CONFIG>"
    -PackageName "${CHROMAROUTE_UWP_PACKAGE_NAME}"
    -Alias "${CHROMAROUTE_UWP_ALIAS}"
    -MakeAppx "${CHROMAROUTE_MAKEAPPX}")
set_tests_properties(uwp_runtime_tests PROPERTIES
  TIMEOUT 900 # a run may first wait up to 300 s for another one (run_uwp_test_package.ps1)
  RUN_SERIAL TRUE
  LABELS "uwp;runtime")
