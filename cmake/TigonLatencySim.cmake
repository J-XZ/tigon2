include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# latency_sim submodule dependency helper for Tigon2.  The tool library is the
# single fixed-latency implementation; this project only consumes it.
#
# Rules enforced here (see latency_sim docs/integration_contract.md and the
# unified migration guide):
#  1. The submodule must exist; otherwise configure fails with the exact
#     restore command (never FetchContent, system package or sibling checkout).
#  2. add_subdirectory(... EXCLUDE_FROM_ALL) runs at most once per CMake
#     process; the same latency_sim::latency_sim instance is shared by every
#     consumer target in that process.
#  3. Subproject mode turns off the tool library's own tests/benchmarks/
#     examples; those are verified by the library repo itself.
#  4. LATENCY_SIM_OPTIMIZATION_PROFILE=optimized and full LTO on the library for
#     Release/RelWithDebInfo (Debug stays -O0 -g3 without LTO).
#  5. No global compiler/flags are modified; the library keeps all options
#     target-local.
#  6. LATENCY_SIM_COMPILE_OFF is the only compile-off gate; it must be
#     ON or OFF and is validated here and in the build scripts.
#  7. LATENCY_SIM_VALGRIND_CHECK is a separate compile-on Debug/O0 variant.
# ---------------------------------------------------------------------------

if(NOT DEFINED LATENCY_SIM_COMPILE_OFF)
  set(LATENCY_SIM_COMPILE_OFF OFF)
endif()
if(NOT LATENCY_SIM_COMPILE_OFF STREQUAL "ON"
   AND NOT LATENCY_SIM_COMPILE_OFF STREQUAL "OFF")
  message(FATAL_ERROR
    "LATENCY_SIM_COMPILE_OFF must be ON or OFF; got '${LATENCY_SIM_COMPILE_OFF}'")
endif()

if(NOT DEFINED LATENCY_SIM_VALGRIND_CHECK)
  set(LATENCY_SIM_VALGRIND_CHECK OFF CACHE BOOL
      "Build the Debug/O0 latencycheck consumer variant")
endif()
if(NOT LATENCY_SIM_VALGRIND_CHECK STREQUAL "ON"
   AND NOT LATENCY_SIM_VALGRIND_CHECK STREQUAL "OFF")
  message(FATAL_ERROR
    "LATENCY_SIM_VALGRIND_CHECK must be ON or OFF; got '${LATENCY_SIM_VALGRIND_CHECK}'")
endif()
if(LATENCY_SIM_VALGRIND_CHECK STREQUAL "ON"
   AND LATENCY_SIM_COMPILE_OFF STREQUAL "ON")
  message(FATAL_ERROR
    "LATENCY_SIM_VALGRIND_CHECK=ON is incompatible with LATENCY_SIM_COMPILE_OFF=ON")
endif()
if(LATENCY_SIM_VALGRIND_CHECK STREQUAL "ON"
   AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR
    "LATENCY_SIM_VALGRIND_CHECK=ON is restricted to the Debug build type with -O0")
endif()

set(TIGONKV_LATENCY_SIM_SOURCE_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../thirdparty_libs/latency_sim")

function(tigonkv_enable_latency_sim)
  if(NOT EXISTS "${TIGONKV_LATENCY_SIM_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "latency_sim submodule missing at ${TIGONKV_LATENCY_SIM_SOURCE_DIR}. "
      "Run: git submodule update --init --recursive thirdparty_libs/latency_sim")
  endif()
  if(TARGET latency_sim::latency_sim)
    return()
  endif()
  set(LATENCY_SIM_BUILD_TESTING OFF CACHE BOOL
      "latency_sim tests are verified by the library repo itself" FORCE)
  set(LATENCY_SIM_BUILD_BENCHMARKS OFF CACHE BOOL
      "latency_sim benchmarks are verified by the library repo itself" FORCE)
  set(LATENCY_SIM_BUILD_EXAMPLES OFF CACHE BOOL
      "latency_sim examples are verified by the library repo itself" FORCE)
  set(LATENCY_SIM_OPTIMIZATION_PROFILE "optimized" CACHE STRING
      "latency_sim optimization profile used by Tigon2 consumers" FORCE)
  set(LATENCY_SIM_ENABLE_LTO ON CACHE BOOL
      "Full LTO on latency_sim and its final consumers (Tigon2 policy)" FORCE)
  include("${TIGONKV_LATENCY_SIM_SOURCE_DIR}/cmake/LatencySimBuildOptions.cmake")
  add_subdirectory("${TIGONKV_LATENCY_SIM_SOURCE_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/latency_sim_build"
                   EXCLUDE_FROM_ALL)
  latency_sim_remove_implicit_optimization_flags()
  if(LATENCY_SIM_VALGRIND_CHECK STREQUAL "ON")
    # The submodule is built in this consumer's Debug checker variant too;
    # keep its target-local debug info readable by the pinned Valgrind 3.18.1.
    target_compile_options(latency_sim PRIVATE -gdwarf-4)
  endif()
endfunction()
