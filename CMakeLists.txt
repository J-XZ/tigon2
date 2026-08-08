cmake_minimum_required(VERSION 3.16)

include(cmake/TigonBuildOptions.cmake)
include(cmake/TigonLatencySim.cmake)

# Compiler selection must happen before project() so the default clang-18
# toolchain (and its ccache launcher / LLVM archive tools) is honored.
tigonkv_select_default_toolchain()

project(tigon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
enable_testing()

# LATENCY_SIM_COMPILE_OFF is the only compile-off gate.  Default is OFF; ON
# requires an independent build directory so no translation unit ever
# mixes compile-off and runtime public-header definitions.
set(LATENCY_SIM_COMPILE_OFF "OFF" CACHE BOOL
  "Compile-time removal of the latency_sim slow path (independent build only)")
set_property(CACHE LATENCY_SIM_COMPILE_OFF PROPERTY STRINGS ON OFF)
if(NOT LATENCY_SIM_COMPILE_OFF STREQUAL "ON"
   AND NOT LATENCY_SIM_COMPILE_OFF STREQUAL "OFF")
  message(FATAL_ERROR
    "LATENCY_SIM_COMPILE_OFF must be ON or OFF; got '${LATENCY_SIM_COMPILE_OFF}'")
endif()
tigonkv_enable_latency_sim()

# Keep the single-config default identical to the documented production
# build before any build-type-specific flags are selected.  An explicit
# caller value, and multi-config generators, remain untouched.
tigonkv_set_default_build_type()

# Full-LTO capability gate: Release/RelWithDebInfo must not silently degrade.
tigonkv_check_lto_support()

# Warning hygiene stays directory-local.  Optimization level, -march=native,
# frame-pointer and LTO flags are project-target policy applied through
# tigonkv_apply_target_build_policy() at the end of this file.
add_compile_options(-Wall -Wextra -Wpedantic
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-variadic-macros
  -Wno-deprecated-declarations)

include_directories(${CMAKE_SOURCE_DIR})

add_library(tigonkv STATIC kv/kv_store.cpp
  core/Table.cpp
  kv/engine/kv_engine.cpp kv/engine/kv_partition.cpp kv/engine/kv_migration.cpp
  kv/engine/region_allocator.cpp
  common/CXLMemory.cpp common/CXL_EBR.cpp common/Time.cpp
  common/btree_olc_cxl/BTreeOLC_CXL.cpp
  protocol/Pasha/SCCManager.cpp protocol/Pasha/MigrationManager.cpp
  protocol/Pasha/PolicyClock.cpp protocol/TwoPLPasha/TwoPLPashaHelper.cpp)
target_link_libraries(tigonkv PUBLIC pthread glog latency_sim::latency_sim)
target_compile_definitions(tigonkv PUBLIC
  TIGONKV_CMAKE_BUILD_TYPE="$<CONFIG>")

# tigonkv: legacy transaction benchmarks retain source-only reference status.
# The redesigned KV target must not link the obsolete cxlalloc implementation.

add_executable(unit_tests tests/tigonkv_tests.cpp)
target_link_libraries(unit_tests PRIVATE tigonkv)
target_compile_definitions(unit_tests PRIVATE
  TIGONKV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_executable(e2e_08 tests/e2e_08.cpp)
target_link_libraries(e2e_08 PRIVATE tigonkv)
add_executable(e2e_09 tests/e2e_09.cpp)
target_link_libraries(e2e_09 PRIVATE tigonkv)
add_executable(e2e_trace_runner tools/e2e_trace_runner.cpp)
target_link_libraries(e2e_trace_runner PRIVATE tigonkv)
add_executable(ycsb_partition_splits tools/ycsb_partition_splits.cpp)
add_executable(latency_modes_test tests/latency_modes_test.cpp)
target_link_libraries(latency_modes_test PRIVATE tigonkv)
add_executable(remote_delete_test tests/remote_delete_latency_test.cpp)
target_link_libraries(remote_delete_test PRIVATE tigonkv)
add_executable(kv_layout_test tests/kv_layout_test.cpp)
target_link_libraries(kv_layout_test PRIVATE tigonkv)
add_executable(btree_binding_test tests/btree_binding_test.cpp)
target_link_libraries(btree_binding_test PRIVATE tigonkv glog)
add_executable(kv_partition_test tests/kv_partition_test.cpp)
target_link_libraries(kv_partition_test PRIVATE tigonkv glog)
add_executable(kv_engine_test tests/kv_engine_test.cpp)
target_link_libraries(kv_engine_test PRIVATE tigonkv glog)
add_executable(kv_startup_test tests/kv_startup_test.cpp)
target_link_libraries(kv_startup_test PRIVATE tigonkv glog)
add_executable(open_failpoint_test tests/open_failpoint_test.cpp)
target_link_libraries(open_failpoint_test PRIVATE tigonkv glog)
add_executable(scc_protocol_test tests/scc_protocol_test.cpp tests/scc_test_backend.cpp)
add_executable(kv_shared_protocol_test tests/kv_shared_protocol_test.cpp)
target_link_libraries(kv_shared_protocol_test PRIVATE tigonkv glog)
add_executable(region_allocator_test tests/region_allocator_test.cpp)
target_link_libraries(region_allocator_test PRIVATE tigonkv)
add_executable(cxl_ebr_test tests/cxl_ebr_test.cpp)
target_link_libraries(cxl_ebr_test PRIVATE tigonkv)
add_executable(numa_placement_probe tools/numa_placement_probe.cpp)
add_executable(cxl_pool_initer tools/cxl_pool_initer.cpp)
add_executable(hardware_sim_disabled_benchmark
               tests/hardware_sim_disabled_benchmark.cpp)
target_link_libraries(hardware_sim_disabled_benchmark PRIVATE tigonkv)
target_compile_options(hardware_sim_disabled_benchmark PRIVATE
                       -mclflushopt -mclwb)

# Every project-owned C/C++ target receives the mode-consistent optimization
# and LTO policy (nothing below may bypass the policy).
set(TIGONKV_PROJECT_TARGETS
  tigonkv
  unit_tests
  e2e_08
  e2e_09
  e2e_trace_runner
  ycsb_partition_splits
  latency_modes_test
  remote_delete_test
  kv_layout_test
  btree_binding_test
  kv_partition_test
  kv_engine_test
  kv_startup_test
  open_failpoint_test
  scc_protocol_test
  kv_shared_protocol_test
  region_allocator_test
  cxl_ebr_test
  numa_placement_probe
  cxl_pool_initer
  hardware_sim_disabled_benchmark
)
foreach(TIGONKV_TARGET IN LISTS TIGONKV_PROJECT_TARGETS)
  tigonkv_apply_target_build_policy(${TIGONKV_TARGET})
endforeach()

# Functionally required ISA features for the targets that compile sources
# reaching the clflushopt/clwb intrinsics in common/MPSCRingBuffer.h and
# protocol/Pasha/SCCManager.h.  These flags exist only because that source
# uses the intrinsics; they are not a general optimization baseline and are
# never applied to third-party/imported targets.
set(TIGONKV_ISA_REQUIRED_TARGETS
  tigonkv
  unit_tests
  e2e_08
  e2e_09
  e2e_trace_runner
  remote_delete_test
  kv_layout_test
  kv_partition_test
  kv_engine_test
  kv_startup_test
  open_failpoint_test
  kv_shared_protocol_test
)
foreach(TIGONKV_TARGET IN LISTS TIGONKV_ISA_REQUIRED_TARGETS)
  target_compile_options(${TIGONKV_TARGET} PRIVATE -mclflushopt -mclwb)
endforeach()

add_custom_target(tigonkv_target ALL DEPENDS tigonkv)
add_custom_target(e2e_trace_target DEPENDS e2e_trace_runner)

add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME ycsb_partition_splits_test
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ycsb_partition_splits_test.sh
                 $<TARGET_FILE:ycsb_partition_splits>)
# Multi-VM × multi-thread guest e2e (cxlkv-aligned: 4 VM × 4 workers via SSH).
add_test(NAME e2e_08_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e_08_test.sh)
add_test(NAME e2e_09_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e_09_test.sh)
add_test(NAME e2e_ycsb_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e_ycsb_test.sh)
# Every guest/QEMU-backed e2e owns shared backing, ports and guest runners.
# RUN_SERIAL is deliberately applied to the whole e2e group so CTest never
# overlaps an e2e with unit tests or another e2e.
set_tests_properties(e2e_08_test e2e_09_test e2e_ycsb_test PROPERTIES
  TIMEOUT 3600
  RUN_SERIAL TRUE)
set_tests_properties(e2e_08_test e2e_09_test e2e_ycsb_test PROPERTIES
  ENVIRONMENT "TIGONKV_E2E_BINARY_DIR=${CMAKE_BINARY_DIR}")
add_test(NAME latency_modes_test COMMAND latency_modes_test)
add_test(NAME remote_delete_test COMMAND remote_delete_test)
add_test(NAME kv_layout_test COMMAND kv_layout_test)
add_test(NAME btree_binding_test COMMAND btree_binding_test)
add_test(NAME kv_partition_test COMMAND kv_partition_test)
add_test(NAME kv_engine_test COMMAND kv_engine_test)
add_test(NAME kv_startup_test COMMAND kv_startup_test)
add_test(NAME open_failpoint_test COMMAND open_failpoint_test)
add_test(NAME scc_protocol_test COMMAND scc_protocol_test)
add_test(NAME kv_shared_protocol_test COMMAND kv_shared_protocol_test)
add_test(NAME region_allocator_test COMMAND region_allocator_test)
add_test(NAME cxl_ebr_test COMMAND cxl_ebr_test)
add_test(NAME vm_scripts_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/vm_scripts_test.sh)
add_test(NAME ycsb_cpp_pin_test COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/tigonkv_ycsb_cpp_pin.sh)
add_test(NAME ycsb_scripts_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ycsb_scripts_test.sh)
add_test(NAME source_state_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/source_state_test.sh)

# The file-backed unit fixtures and the guest/QEMU fixtures must never share
# a test invocation: both can touch the configured backing path, process
# names, or host VM state.  RUN_SERIAL on the guest tests is not enough when a
# caller supplies `ctest -j`; one common CTest resource makes that exclusion
# explicit for every registered test.
get_property(TIGONKV_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${TIGONKV_REGISTERED_TESTS} PROPERTIES
  RESOURCE_LOCK tigonkv_test_environment)
