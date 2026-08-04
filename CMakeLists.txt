cmake_minimum_required(VERSION 3.16)
project(tigon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
enable_testing()

option(TIGONKV_DISABLE_HARDWARE_SIMULATION
       "Compile out all hardware-simulation wrapper slow paths" OFF)

# Keep the single-config default identical to the documented production
# build before any build-type-specific flags are selected.  An explicit
# caller value, and multi-config generators, remain untouched.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -fno-omit-frame-pointer
  -march=native
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-variadic-macros
  -Wno-deprecated-declarations)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-O0 -g3)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  # tigonkv: keep the release-with-symbols contract aligned with the fairness
  # plan.  GCC spells full-program LTO as bare -flto; Clang uses -flto=full.
  add_compile_options(-O3 -g3 -march=native)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-flto)
    add_link_options(-flto)
  else()
    add_compile_options(-flto=full)
    add_link_options(-flto=full)
  endif()
endif()

include_directories(${CMAKE_SOURCE_DIR})

add_library(tigonkv STATIC kv/kv_store.cpp kv/engine/latency_inject.cpp
  core/Table.cpp
  kv/engine/kv_engine.cpp kv/engine/kv_partition.cpp kv/engine/kv_migration.cpp
  kv/engine/region_allocator.cpp
  common/CXLMemory.cpp common/CXL_EBR.cpp common/Time.cpp
  common/btree_olc_cxl/BTreeOLC_CXL.cpp
  protocol/Pasha/SCCManager.cpp protocol/Pasha/MigrationManager.cpp
  protocol/Pasha/PolicyClock.cpp protocol/TwoPLPasha/TwoPLPashaHelper.cpp)
target_link_libraries(tigonkv PUBLIC pthread glog)
target_compile_definitions(tigonkv PUBLIC
  TIGONKV_CMAKE_BUILD_TYPE="$<CONFIG>")
if(TIGONKV_DISABLE_HARDWARE_SIMULATION)
  target_compile_definitions(tigonkv PUBLIC TIGONKV_DISABLE_HARDWARE_SIMULATION=1)
endif()

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
# Compile-time-removal variant: the wrapper fast gate becomes a constant zero,
# so the wrapped loop compiles to the raw atomic operations with no runtime
# branch.  Used only for the disabled-overhead comparison.
add_executable(hardware_sim_disabled_benchmark_compile_off
               tests/hardware_sim_disabled_benchmark.cpp)
target_link_libraries(hardware_sim_disabled_benchmark_compile_off PRIVATE tigonkv)
target_compile_definitions(hardware_sim_disabled_benchmark_compile_off
                           PRIVATE TIGONKV_DISABLE_HARDWARE_SIMULATION=1)

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
add_test(NAME e2e_ycsb_entrypoints_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e_ycsb_entrypoints_test.sh)
# Every guest/QEMU-backed e2e owns shared backing, ports and guest runners.
# RUN_SERIAL is deliberately applied to the whole e2e group so CTest never
# overlaps an e2e with unit tests or another e2e.
set_tests_properties(e2e_08_test e2e_09_test e2e_ycsb_test
                     e2e_ycsb_entrypoints_test PROPERTIES
  TIMEOUT 3600
  RUN_SERIAL TRUE)
set_tests_properties(e2e_08_test e2e_09_test e2e_ycsb_test PROPERTIES
  ENVIRONMENT "TIGONKV_E2E_BINARY_DIR=${CMAKE_BINARY_DIR}")
add_test(NAME latency_modes_test COMMAND latency_modes_test)
add_test(NAME kv_layout_test COMMAND kv_layout_test)
add_test(NAME btree_binding_test COMMAND btree_binding_test)
add_test(NAME kv_partition_test COMMAND kv_partition_test)
add_test(NAME kv_engine_test COMMAND kv_engine_test)
add_test(NAME kv_startup_test COMMAND kv_startup_test)
add_test(NAME scc_protocol_test COMMAND scc_protocol_test)
add_test(NAME kv_shared_protocol_test COMMAND kv_shared_protocol_test)
add_test(NAME region_allocator_test COMMAND region_allocator_test)
add_test(NAME cxl_ebr_test COMMAND cxl_ebr_test)
add_test(NAME vm_scripts_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/vm_scripts_test.sh)
add_test(NAME ycsb_cpp_pin_test COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/tigonkv_ycsb_cpp_pin.sh)
add_test(NAME ycsb_scripts_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ycsb_scripts_test.sh)

# The file-backed unit fixtures and the guest/QEMU fixtures must never share
# a test invocation: both can touch the configured backing path, process
# names, or host VM state.  RUN_SERIAL on the guest tests is not enough when a
# caller supplies `ctest -j`; one common CTest resource makes that exclusion
# explicit for every registered test.
get_property(TIGONKV_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${TIGONKV_REGISTERED_TESTS} PROPERTIES
  RESOURCE_LOCK tigonkv_test_environment)
