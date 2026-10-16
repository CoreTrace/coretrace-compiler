# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# Pinned release; keep in sync with the version noted in the README.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.18.0
  EXCLUDE_FROM_ALL
)
set(INSTALL_GTEST OFF CACHE BOOL "Do not install GoogleTest with the project" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "GoogleMock is not used" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(compilerlib_unit_tests
  alloc_names_test.cpp
  alloc_pass_test.cpp
  args_test.cpp
  bounds_pass_test.cpp
  compile_test.cpp
  toolchain_test.cpp
  vtable_pass_test.cpp
)
target_include_directories(compilerlib_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
# Fixtures that compile_test.cpp compiles in process.
target_compile_definitions(compilerlib_unit_tests PRIVATE
  CT_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test")
target_link_libraries(compilerlib_unit_tests PRIVATE compilerlib_static GTest::gtest_main)
coretrace_force_msvc_runtime(compilerlib_unit_tests)
coretrace_disable_rtti(compilerlib_unit_tests)

if(TARGET ct_instrument_runtime)
  # The runtime's allocation table, linked as instrumented programs link it. The
  # runtime installs its constructors and leak report in this process; every test
  # releases what it inserts.
  add_executable(runtime_unit_tests
    runtime_bounds_test.cpp
    runtime_block_index_test.cpp
    runtime_demangle_cache_test.cpp
    runtime_demangle_test.cpp
    runtime_leak_report_test.cpp
    runtime_log_output_test.cpp
    runtime_quarantine_test.cpp
    runtime_scope_test.cpp
    runtime_table_test.cpp
    runtime_vtable_reload_test.cpp
  )
  target_include_directories(runtime_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
  target_link_libraries(runtime_unit_tests PRIVATE ct_instrument_runtime coretrace_logger GTest::gtest_main
    ${CMAKE_DL_LIBS})
  # Two builds of one library, with names of their own, that runtime_vtable_reload_test.cpp
  # loads, unloads and loads again.
  foreach(build a b)
    add_library(ct_reload_${build} MODULE reload/ct_reload_library.cpp)
    target_compile_definitions(ct_reload_${build} PRIVATE CT_RELOAD_NAMESPACE=reload_${build})
    # Without the sanitizers' references, as the runtime: a copy loaded with dlmopen, in a
    # namespace of its own, cannot reach the runtimes the test program links.
    if(ENABLE_DEBUG_ASAN)
      target_compile_options(ct_reload_${build} PRIVATE -fno-sanitize=address)
    endif()
    if(ENABLE_DEBUG_UBSAN)
      target_compile_options(ct_reload_${build} PRIVATE -fno-sanitize=undefined)
    endif()
    coretrace_force_msvc_runtime(ct_reload_${build})
    add_dependencies(runtime_unit_tests ct_reload_${build})
  endforeach()
  target_compile_definitions(runtime_unit_tests PRIVATE
    CT_RELOAD_LIBRARY_A="$<TARGET_FILE:ct_reload_a>"
    CT_RELOAD_LIBRARY_B="$<TARGET_FILE:ct_reload_b>")
  if(NOT WIN32)
    # The module comparison of the POSIX vtable diagnostics.
    target_sources(runtime_unit_tests PRIVATE runtime_vtable_modules_test.cpp)
    # The order of the runtime's locks and the logger's, which both take around fork().
    target_sources(runtime_unit_tests PRIVATE runtime_log_locks_test.cpp)
    # The allocation table filled up, in a process apart from the other runtime tests.
    add_executable(runtime_table_full_tests runtime_table_full_test.cpp)
    target_include_directories(runtime_table_full_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(runtime_table_full_tests PRIVATE ct_instrument_runtime coretrace_logger
      GTest::gtest_main)
    # Tombstones left by a table filled up and emptied, in a process apart.
    add_executable(runtime_table_tombstones_tests runtime_table_tombstones_test.cpp)
    target_include_directories(runtime_table_tombstones_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(runtime_table_tombstones_tests PRIVATE ct_instrument_runtime
      coretrace_logger GTest::gtest_main)
    # The fallback of the runtime's sink, in a process whose operator new it replaces.
    add_executable(runtime_log_fallback_tests runtime_log_fallback_test.cpp)
    target_include_directories(runtime_log_fallback_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(runtime_log_fallback_tests PRIVATE ct_instrument_runtime coretrace_logger
      GTest::gtest_main)
  endif()
  if(APPLE)
    # Objective-C object tracking: pulls ct_runtime_objc.cpp, which calls into libobjc.
    target_sources(runtime_unit_tests PRIVATE runtime_objc_test.cpp)
    target_link_libraries(runtime_unit_tests PRIVATE objc)
    # How the auto-free scans stop the threads, in a process where the scan is enabled.
    add_executable(runtime_scan_threads_tests runtime_scan_threads_test.cpp)
    target_include_directories(runtime_scan_threads_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(runtime_scan_threads_tests PRIVATE ct_instrument_runtime coretrace_logger
      GTest::gtest_main)
  endif()
  coretrace_force_msvc_runtime(runtime_unit_tests)
endif()

include(GoogleTest)
gtest_discover_tests(compilerlib_unit_tests)
if(TARGET runtime_unit_tests)
  gtest_discover_tests(runtime_unit_tests)
endif()
if(TARGET runtime_table_full_tests)
  gtest_discover_tests(runtime_table_full_tests)
endif()
if(TARGET runtime_table_tombstones_tests)
  gtest_discover_tests(runtime_table_tombstones_tests)
endif()
if(TARGET runtime_scan_threads_tests)
  gtest_discover_tests(runtime_scan_threads_tests PROPERTIES ENVIRONMENT CT_AUTOFREE_SCAN=1)
endif()
if(TARGET runtime_log_fallback_tests)
  # A limit outside the test: a writer that blocks also blocks the test's future in its
  # destructor, past the test's own wait.
  gtest_discover_tests(runtime_log_fallback_tests PROPERTIES TIMEOUT 120)
endif()
