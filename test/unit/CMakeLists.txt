# SPDX-License-Identifier: Apache-2.0
include(FetchContent)

# Pinned release; keep in sync with the version noted in the README.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  EXCLUDE_FROM_ALL
)
set(INSTALL_GTEST OFF CACHE BOOL "Do not install GoogleTest with the project" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "GoogleMock is not used" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(compilerlib_unit_tests
  alloc_names_test.cpp
  args_test.cpp
)
target_include_directories(compilerlib_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(compilerlib_unit_tests PRIVATE compilerlib_static GTest::gtest_main)
coretrace_force_msvc_runtime(compilerlib_unit_tests)
coretrace_disable_rtti(compilerlib_unit_tests)

# The runtime's allocation table, linked as instrumented programs link it. The
# runtime installs its constructors and leak report in this process; every test
# releases what it inserts.
add_executable(runtime_unit_tests
  runtime_bounds_test.cpp
  runtime_demangle_test.cpp
  runtime_table_test.cpp
)
target_include_directories(runtime_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
target_link_libraries(runtime_unit_tests PRIVATE ct_instrument_runtime coretrace_logger GTest::gtest_main)
coretrace_force_msvc_runtime(runtime_unit_tests)

include(GoogleTest)
gtest_discover_tests(compilerlib_unit_tests)
gtest_discover_tests(runtime_unit_tests)
