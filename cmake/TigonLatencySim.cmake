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
#  4. LATENCY_SIM_OPTIMIZATION_PROFILE=cxlkv and full LTO on the library for
#     Release/RelWithDebInfo (Debug stays -O0 -g3 without LTO).
#  5. No global compiler/flags are modified; the library keeps all options
#     target-local.
#  6. LATENCY_SIM_COMPILE_OFF is the only compile-off gate; it must be
#     ON or OFF and is validated here and in the build scripts.
# ---------------------------------------------------------------------------

if(NOT DEFINED LATENCY_SIM_COMPILE_OFF)
  set(LATENCY_SIM_COMPILE_OFF OFF)
endif()
if(NOT LATENCY_SIM_COMPILE_OFF STREQUAL "ON"
   AND NOT LATENCY_SIM_COMPILE_OFF STREQUAL "OFF")
  message(FATAL_ERROR
    "LATENCY_SIM_COMPILE_OFF must be ON or OFF; got '${LATENCY_SIM_COMPILE_OFF}'")
endif()

set(TIGONKV_LATENCY_SIM_SOURCE_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../thirdparty_libs/latency_sim")

get_filename_component(TIGONKV_PROJECT_SOURCE_DIR
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Bind consumer identities to the indexed public gitlink, not merely to the
# checkout currently present in the submodule directory.
if(NOT DEFINED TIGONKV_LATENCY_SIM_PUBLIC_GITLINK)
  execute_process(
    COMMAND git -C "${TIGONKV_PROJECT_SOURCE_DIR}" rev-parse
            HEAD:thirdparty_libs/latency_sim
    OUTPUT_VARIABLE TIGONKV_LATENCY_SIM_PUBLIC_GITLINK
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _tigonkv_latency_sim_gitlink_result
    ERROR_QUIET)
  if(NOT _tigonkv_latency_sim_gitlink_result EQUAL 0
     OR TIGONKV_LATENCY_SIM_PUBLIC_GITLINK STREQUAL "")
    message(FATAL_ERROR
      "cannot resolve the indexed latency_sim gitlink from ${TIGONKV_PROJECT_SOURCE_DIR}")
  endif()
endif()

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
  set(LATENCY_SIM_OPTIMIZATION_PROFILE "cxlkv" CACHE STRING
      "latency_sim optimization profile used by Tigon2 consumers" FORCE)
  set(LATENCY_SIM_ENABLE_LTO ON CACHE BOOL
      "Full LTO on latency_sim and its final consumers (Tigon2 policy)" FORCE)
  add_subdirectory("${TIGONKV_LATENCY_SIM_SOURCE_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/latency_sim_build"
                   EXCLUDE_FROM_ALL)
endfunction()

# Attach a public latency_sim V8 consumer identity object to a final binary.
# The query header is side-effect free and is called before any VM, pool or
# network setup in each executable that participates in final evidence.
function(tigonkv_add_build_identity target role)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "tigonkv_add_build_identity target does not exist: ${target}")
  endif()
  if(NOT role)
    message(FATAL_ERROR "tigonkv_add_build_identity requires a target role")
  endif()
  if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(_tigonkv_identity_build_type "Unknown")
  else()
    set(_tigonkv_identity_build_type "${CMAKE_BUILD_TYPE}")
  endif()
  if(_tigonkv_identity_build_type STREQUAL "Debug")
    set(_tigonkv_identity_lto "OFF")
  else()
    set(_tigonkv_identity_lto "ON")
  endif()
  set(_tigonkv_identity_variant
    "role=${role},build_type=${_tigonkv_identity_build_type},compile_off=${LATENCY_SIM_COMPILE_OFF},compiler=${CMAKE_CXX_COMPILER},lto=${_tigonkv_identity_lto},profile=cxlkv")
  latency_sim_add_build_identity(${target}
    PARENT_SOURCE_DIR "${TIGONKV_PROJECT_SOURCE_DIR}"
    PUBLIC_GITLINK "${TIGONKV_LATENCY_SIM_PUBLIC_GITLINK}"
    VARIANT "${_tigonkv_identity_variant}"
    PROJECT_ID "${role}")
  string(MAKE_C_IDENTIFIER "${target}" _tigonkv_identity_identifier)
  set(_tigonkv_identity_namespace
    "latency_sim_consumer_identity_${_tigonkv_identity_identifier}")
  target_compile_definitions(${target} PRIVATE
    TIGONKV_BUILD_IDENTITY_NAMESPACE=${_tigonkv_identity_namespace})
  if(LATENCY_SIM_COMPILE_OFF STREQUAL "ON")
    target_compile_definitions(${target} PRIVATE LATENCY_SIM_COMPILE_OFF)
  endif()
  target_include_directories(${target} PRIVATE
    "${TIGONKV_PROJECT_SOURCE_DIR}/tools")
endfunction()
