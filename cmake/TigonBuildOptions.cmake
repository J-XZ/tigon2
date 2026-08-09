include_guard(GLOBAL)

include(CheckIPOSupported)

option(TIGONKV_ENABLE_FRAME_POINTERS
  "Keep frame pointers for profiler-friendly optimized builds" OFF)

# Defaults for the single-config production build.  Callers that explicitly
# select CMAKE_BUILD_TYPE or use a multi-config generator are never overridden.
function(tigonkv_set_default_build_type)
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  endif()
endfunction()

# Select the default toolchain.  MUST run before project(): callers that
# explicitly picked a compiler keep their choice; otherwise the CXLKV-aligned
# clang-18 / clang++-18 toolchain is selected.  ccache is used as the C/C++
# compiler launcher when present (never an error when absent).  With a Clang
# toolchain the matching LLVM archive/index tools (llvm-ar-18 / llvm-ranlib-18)
# and ld.lld-18 are preferred so LTO bitcode archives and links never mix with
# incompatible binutils tools.
function(tigonkv_select_default_toolchain)
  if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(_tigonkv_default_c_compiler clang-18)
    if(_tigonkv_default_c_compiler)
      set(CMAKE_C_COMPILER "${_tigonkv_default_c_compiler}" CACHE FILEPATH "" FORCE)
    else()
      message(FATAL_ERROR
        "clang-18 not found; install the LLVM 18 toolchain or set "
        "CMAKE_C_COMPILER explicitly")
    endif()
  endif()
  if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(_tigonkv_default_cxx_compiler clang++-18)
    if(_tigonkv_default_cxx_compiler)
      set(CMAKE_CXX_COMPILER "${_tigonkv_default_cxx_compiler}" CACHE FILEPATH "" FORCE)
    else()
      message(FATAL_ERROR
        "clang++-18 not found; install the LLVM 18 toolchain or set "
        "CMAKE_CXX_COMPILER explicitly")
    endif()
  endif()

  find_program(_tigonkv_ccache ccache REQUIRED)
  foreach(_tigonkv_launcher CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER)
    if(DEFINED ${_tigonkv_launcher}
       AND NOT "${${_tigonkv_launcher}}" STREQUAL ""
       AND NOT "${${_tigonkv_launcher}}" MATCHES "(^|/)ccache$")
      message(FATAL_ERROR
        "${_tigonkv_launcher} must use ccache; got '${${_tigonkv_launcher}}'")
    endif()
    set(${_tigonkv_launcher} "${_tigonkv_ccache}" CACHE STRING
      "Required compiler launcher" FORCE)
    set(${_tigonkv_launcher} "${_tigonkv_ccache}" PARENT_SCOPE)
  endforeach()

  set(_tigonkv_clang_selected "")
  if(DEFINED CMAKE_C_COMPILER)
    get_filename_component(_tigonkv_c_compiler_name "${CMAKE_C_COMPILER}" NAME)
    string(REGEX MATCH "^clang" _tigonkv_clang_selected "${_tigonkv_c_compiler_name}")
  endif()
  if(DEFINED CMAKE_CXX_COMPILER AND NOT _tigonkv_clang_selected)
    get_filename_component(_tigonkv_cxx_compiler_name "${CMAKE_CXX_COMPILER}" NAME)
    string(REGEX MATCH "^clang" _tigonkv_clang_selected "${_tigonkv_cxx_compiler_name}")
  endif()
  if(_tigonkv_clang_selected)
    if(NOT DEFINED CMAKE_C_COMPILER_AR OR NOT DEFINED CMAKE_CXX_COMPILER_AR)
      find_program(_tigonkv_llvm_ar NAMES llvm-ar-18 llvm-ar)
      if(_tigonkv_llvm_ar)
        if(NOT DEFINED CMAKE_C_COMPILER_AR)
          set(CMAKE_C_COMPILER_AR "${_tigonkv_llvm_ar}" PARENT_SCOPE)
        endif()
        if(NOT DEFINED CMAKE_CXX_COMPILER_AR)
          set(CMAKE_CXX_COMPILER_AR "${_tigonkv_llvm_ar}" PARENT_SCOPE)
        endif()
      endif()
    endif()
    if(NOT DEFINED CMAKE_C_COMPILER_RANLIB OR NOT DEFINED CMAKE_CXX_COMPILER_RANLIB)
      find_program(_tigonkv_llvm_ranlib NAMES llvm-ranlib-18 llvm-ranlib)
      if(_tigonkv_llvm_ranlib)
        if(NOT DEFINED CMAKE_C_COMPILER_RANLIB)
          set(CMAKE_C_COMPILER_RANLIB "${_tigonkv_llvm_ranlib}" PARENT_SCOPE)
        endif()
        if(NOT DEFINED CMAKE_CXX_COMPILER_RANLIB)
          set(CMAKE_CXX_COMPILER_RANLIB "${_tigonkv_llvm_ranlib}" PARENT_SCOPE)
        endif()
      endif()
    endif()
    if(NOT DEFINED CMAKE_LINKER)
      find_program(_tigonkv_ld_lld NAMES ld.lld-18 ld.lld)
      if(_tigonkv_ld_lld)
        set(CMAKE_LINKER "${_tigonkv_ld_lld}" PARENT_SCOPE)
      endif()
    endif()
  endif()

  set(CMAKE_EXPORT_COMPILE_COMMANDS ON PARENT_SCOPE)
endfunction()

# Full-LTO capability gate.  Runs after project(); fails the configure step
# with a clear error instead of silently degrading the performance builds.
function(tigonkv_check_lto_support)
  check_ipo_supported(RESULT _tigonkv_ipo_supported OUTPUT _tigonkv_ipo_error
    LANGUAGES C CXX)
  if(NOT _tigonkv_ipo_supported)
    message(FATAL_ERROR
      "IPO/LTO is not supported by the selected toolchain: ${_tigonkv_ipo_error}")
  endif()
endfunction()

# Apply the optimization/debug and compile-off policy to one project-owned
# target.  The cache flags above remove CMake's implicit optimization choices;
# these target options are the exact project policy.
#
#   Debug:         -O0 -g3, no -march=native, no LTO
#   RelWithDebInfo:-O3 -g3 -march=native, -flto=full (GCC: -flto), -DNDEBUG
#   Release:       -O3 -march=native, -flto=full (GCC: -flto),
#                  -DNDEBUG
#   Checker Debug: -O0 -g3, compile-on, checker-on.  NDEBUG is selected only
#                  by tigonkv_apply_e2e_participant_policy().
function(tigonkv_apply_target_build_policy target_name)
  if(LATENCY_SIM_COMPILE_OFF STREQUAL "ON")
    target_compile_definitions(${target_name} PRIVATE LATENCY_SIM_COMPILE_OFF)
  endif()
  target_compile_options(${target_name} PRIVATE
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-gdwarf-4>
    $<$<CONFIG:RelWithDebInfo>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-g3>
    $<$<CONFIG:RelWithDebInfo>:-march=native>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
  )
  if(LATENCY_SIM_VALGRIND_CHECK STREQUAL "ON")
    # Valgrind cover is never valid against an optimized consumer.  Keep the
    # flags target-local so a checker build cannot alter another CMake target.
    # Valgrind 3.18.1 cannot read the newer DWARF forms emitted by clang-18;
    # DWARF-4 keeps the Debug+O0 checker artifact readable without changing
    # the production build policy.
    target_compile_options(${target_name} PRIVATE -O0 -g3 -gdwarf-4)
  endif()
  if(TIGONKV_ENABLE_FRAME_POINTERS)
    target_compile_options(${target_name} PRIVATE -fno-omit-frame-pointer)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_tigonkv_lto_flag "-flto")
  else()
    set(_tigonkv_lto_flag "-flto=full")
  endif()
  if(NOT LATENCY_SIM_VALGRIND_CHECK STREQUAL "ON")
    target_compile_options(${target_name} PRIVATE
      $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:${_tigonkv_lto_flag}>
    )
    target_link_options(${target_name} PRIVATE
      $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:${_tigonkv_lto_flag}>
    )
  endif()
endfunction()

# Thin project wrapper around the generic policy shipped by latency_sim.
function(tigonkv_apply_e2e_participant_policy target_name)
  if(NOT LATENCY_SIM_E2E_NDEBUG STREQUAL "ON")
    return()
  endif()
  if(NOT COMMAND latency_sim_apply_e2e_participant_policy)
    message(FATAL_ERROR
      "latency_sim participant policy is unavailable; enable the submodule first")
  endif()
  latency_sim_apply_e2e_participant_policy(${target_name})
endfunction()
