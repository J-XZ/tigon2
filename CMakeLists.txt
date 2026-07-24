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

include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/dependencies/cxlalloc)
find_library(jemalloc_lib jemalloc)

file(GLOB_RECURSE MISC_CPP_FILES CONFIGURE_DEPENDS
  common/*.cpp protocol/Pasha/*.cpp protocol/SundialPasha/*.cpp
  protocol/TwoPLPasha/*.cpp core/*.cpp)
add_library(misc_cpp STATIC ${MISC_CPP_FILES})

add_library(tigonkv STATIC kv/kv_store.cpp kv/latency_simulator.cpp
  kv/engine/region_allocator.cpp)
target_link_libraries(tigonkv PUBLIC pthread)

add_executable(bench_tpcc bench_tpcc.cpp)
target_link_libraries(bench_tpcc misc_cpp ${CMAKE_SOURCE_DIR}/dependencies/cxlalloc/libcxlalloc_static.a ${jemalloc_lib} glog gflags)
add_executable(bench_ycsb bench_ycsb.cpp)
target_link_libraries(bench_ycsb misc_cpp ${CMAKE_SOURCE_DIR}/dependencies/cxlalloc/libcxlalloc_static.a ${jemalloc_lib} glog gflags)

add_executable(unit_tests tests/tigonkv_tests.cpp)
target_link_libraries(unit_tests PRIVATE tigonkv)
add_executable(allocator_tests tests/tigonkv_tests.cpp)
target_link_libraries(allocator_tests PRIVATE tigonkv)
set(ALLOCATOR_TEST_TARGETS
  allocator_capability_test allocator_multi_process_test allocator_multi_vm_test
  allocator_restart_attach_test allocator_global_pool_test allocator_concurrency_test
  allocator_reclaim_test allocator_accounting_test allocator_domain_budget_test
  allocator_local_dram_test)
foreach(test_target IN LISTS ALLOCATOR_TEST_TARGETS)
  add_executable(${test_target} tests/region_allocator_test.cpp)
  target_link_libraries(${test_target} PRIVATE tigonkv)
endforeach()
add_executable(e2e_tests tests/tigonkv_tests.cpp)
target_link_libraries(e2e_tests PRIVATE tigonkv)
set(TIGONKV_CONFIG_TEST_TARGETS unit_tests allocator_tests e2e_tests ${ALLOCATOR_TEST_TARGETS})
foreach(test_target IN LISTS TIGONKV_CONFIG_TEST_TARGETS)
  target_compile_definitions(${test_target} PRIVATE TIGONKV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endforeach()
add_executable(e2e_08 tests/e2e_08.cpp)
target_link_libraries(e2e_08 PRIVATE tigonkv)
add_executable(e2e_09 tests/e2e_09.cpp)
target_link_libraries(e2e_09 PRIVATE tigonkv)
add_executable(e2e_trace tools/e2e_trace_runner.cpp)
target_link_libraries(e2e_trace PRIVATE tigonkv)
add_executable(e2e_trace_runner tools/e2e_trace_runner.cpp)
target_link_libraries(e2e_trace_runner PRIVATE tigonkv)
add_executable(latency_modes_test tests/latency_modes_test.cpp)
target_link_libraries(latency_modes_test PRIVATE tigonkv)
add_executable(kv_layout_test tests/kv_layout_test.cpp)
target_link_libraries(kv_layout_test PRIVATE tigonkv)
add_executable(region_allocator_test tests/region_allocator_test.cpp)
target_link_libraries(region_allocator_test PRIVATE tigonkv)
add_executable(numa_placement_probe tools/numa_placement_probe.cpp)
add_executable(cxl_pool_initer tools/cxl_pool_initer.cpp)

add_custom_target(tigonkv_target ALL DEPENDS tigonkv)
add_custom_target(e2e_trace_target DEPENDS e2e_trace_runner)

add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME allocator_capability_test COMMAND allocator_capability_test)
add_test(NAME allocator_multi_process_test COMMAND allocator_multi_process_test)
add_test(NAME allocator_multi_vm_test COMMAND allocator_multi_vm_test)
add_test(NAME allocator_restart_attach_test COMMAND allocator_restart_attach_test)
add_test(NAME allocator_global_pool_test COMMAND allocator_global_pool_test)
add_test(NAME allocator_concurrency_test COMMAND allocator_concurrency_test)
add_test(NAME allocator_reclaim_test COMMAND allocator_reclaim_test)
add_test(NAME allocator_accounting_test COMMAND allocator_accounting_test)
add_test(NAME allocator_domain_budget_test COMMAND allocator_domain_budget_test)
add_test(NAME allocator_local_dram_test COMMAND allocator_local_dram_test)
add_test(NAME e2e_tests COMMAND e2e_tests)
add_test(NAME e2e_08_test COMMAND e2e_08)
add_test(NAME e2e_09_test COMMAND e2e_09)
add_test(NAME e2e_trace_test COMMAND e2e_trace)
add_test(NAME latency_modes_test COMMAND latency_modes_test)
add_test(NAME kv_layout_test COMMAND kv_layout_test)
add_test(NAME region_allocator_test COMMAND region_allocator_test)
set_tests_properties(e2e_trace_test PROPERTIES
  ENVIRONMENT
    "TIGONKV_EXPERIMENT_CONFIG_JSONC=${CMAKE_SOURCE_DIR}/tests/fixtures/experiment_config.jsonc;TIGONKV_E2E_TRACE_FILE=${CMAKE_SOURCE_DIR}/tests/fixtures/trace.txt;TIGONKV_E2E_RESET=1;TIGONKV_E2E_TRACE_PHASE=fixture")
