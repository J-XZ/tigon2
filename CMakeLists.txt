cmake_minimum_required(VERSION 3.16)
project(tigon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CTest)
enable_testing()

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
  else()
    add_compile_options(-flto=full)
  endif()
elseif(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include_directories(${CMAKE_SOURCE_DIR})

add_library(tigonkv STATIC kv/kv_store.cpp kv/engine/latency_inject.cpp
  kv/engine/kv_engine.cpp kv/engine/kv_partition.cpp kv/engine/kv_migration.cpp
  kv/engine/region_allocator.cpp
  common/CXLMemory.cpp common/CXL_EBR.cpp common/btree_olc_cxl/BTreeOLC_CXL.cpp
  protocol/Pasha/SCCManager.cpp protocol/Pasha/MigrationManager.cpp
  protocol/Pasha/PolicyClock.cpp)
target_link_libraries(tigonkv PUBLIC pthread glog)
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
add_executable(numa_placement_probe tools/numa_placement_probe.cpp)
add_executable(cxl_pool_initer tools/cxl_pool_initer.cpp)

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
set_tests_properties(e2e_08_test e2e_09_test PROPERTIES
  TIMEOUT 3600
  RUN_SERIAL TRUE)
add_test(NAME latency_modes_test COMMAND latency_modes_test)
add_test(NAME kv_layout_test COMMAND kv_layout_test)
add_test(NAME btree_binding_test COMMAND btree_binding_test)
add_test(NAME kv_partition_test COMMAND kv_partition_test)
add_test(NAME kv_engine_test COMMAND kv_engine_test)
add_test(NAME kv_startup_test COMMAND kv_startup_test)
add_test(NAME scc_protocol_test COMMAND scc_protocol_test)
add_test(NAME kv_shared_protocol_test COMMAND kv_shared_protocol_test)
add_test(NAME region_allocator_test COMMAND region_allocator_test)
add_test(NAME vm_scripts_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/vm_scripts_test.sh)
add_test(NAME ycsb_cpp_pin_test COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/tigonkv_ycsb_cpp_pin.sh)
add_test(NAME ycsb_scripts_test COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ycsb_scripts_test.sh)
