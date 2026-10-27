# Target-scoped compiler options shared by every LibChromaRoute target.

function(chromaroute_apply_compiler_options target)
  if(MSVC)
    # /utf-8 fixes the source and execution character sets; /fp:precise keeps
    # floating-point results reproducible (decision N3). /permissive- and
    # /Zc:__cplusplus give standard-conforming C++.
    target_compile_options(${target} PRIVATE
      /W4 /WX /utf-8 /fp:precise /permissive- /Zc:__cplusplus)
  else()
    # No FMA contraction: fused operations would change reference results
    # between platforms (decision N3).
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -ffp-contract=off)
  endif()
endfunction()
