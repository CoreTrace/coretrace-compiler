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
if(NOT WIN32)
  # Compilations from threads with small stacks, in subprocesses (pthread_attr_setstacksize).
  target_sources(compilerlib_unit_tests PRIVATE compile_stack_test.cpp)
endif()
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
    # The symbol of a call target, in a program whose code and symbols are in segments apart on
    # Linux, as with 64 KiB pages (#223).
    add_executable(runtime_vtable_symbol_tests runtime_vtable_symbol_test.cpp)
    target_include_directories(runtime_vtable_symbol_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(runtime_vtable_symbol_tests PRIVATE ct_instrument_runtime coretrace_logger
      GTest::gtest_main ${CMAKE_DL_LIBS})
    target_compile_definitions(runtime_vtable_symbol_tests PRIVATE
      CT_RELOAD_LIBRARY_A="$<TARGET_FILE:ct_reload_a>")
    add_dependencies(runtime_vtable_symbol_tests ct_reload_a)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Exported on Linux, where the runtime reads the program's dynamic symbols: macOS's dladdr
      # names the program's functions without.
      set_target_properties(runtime_vtable_symbol_tests PROPERTIES ENABLE_EXPORTS ON)
      target_link_options(runtime_vtable_symbol_tests PRIVATE -Wl,-z,separate-code
        -Wl,-z,max-page-size=0x10000)
      # A build of the library with a DT_HASH table and no GNU hash table.
      add_library(ct_reload_sysv MODULE reload/ct_reload_library.cpp)
      target_compile_definitions(ct_reload_sysv PRIVATE CT_RELOAD_NAMESPACE=reload_sysv)
      target_link_options(ct_reload_sysv PRIVATE -Wl,--hash-style=sysv)
      target_compile_definitions(runtime_vtable_symbol_tests PRIVATE
        CT_RELOAD_LIBRARY_SYSV="$<TARGET_FILE:ct_reload_sysv>")
      add_dependencies(runtime_vtable_symbol_tests ct_reload_sysv)
    endif()
    # The order of the runtime's locks and the logger's, which both take around fork().
    target_sources(runtime_unit_tests PRIVATE runtime_log_locks_test.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # A child of fork() that looks modules up while the parent was listing them (#206).
      target_sources(runtime_unit_tests PRIVATE runtime_vtable_fork_test.cpp)
      # A runtime that never lists the modules, in a child of fork() or in its parent: a test
      # program of its own, which replaces dl_iterate_phdr for the runtime it links (#206).
      add_executable(runtime_fork_modules_tests runtime_fork_modules_test.cpp)
      target_include_directories(runtime_fork_modules_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
      target_link_libraries(runtime_fork_modules_tests PRIVATE ct_instrument_runtime coretrace_logger
        GTest::gtest_main ${CMAKE_DL_LIBS})
      target_compile_definitions(runtime_fork_modules_tests PRIVATE
        CT_RELOAD_LIBRARY_A="$<TARGET_FILE:ct_reload_a>"
        CT_RELOAD_LIBRARY_B="$<TARGET_FILE:ct_reload_b>")
      add_dependencies(runtime_fork_modules_tests ct_reload_a ct_reload_b)
      # A library whose constructor waits for traced virtual calls, loaded by a program that
      # exports the runtime's entry points to it (#223).
      add_library(ct_trace_constructor_library MODULE reload/ct_trace_constructor_library.cpp)
      add_executable(runtime_loader_lock_tests runtime_loader_lock_test.cpp)
      set_target_properties(runtime_loader_lock_tests PROPERTIES ENABLE_EXPORTS ON)
      target_include_directories(runtime_loader_lock_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
      target_link_libraries(runtime_loader_lock_tests PRIVATE ct_instrument_runtime coretrace_logger
        GTest::gtest_main ${CMAKE_DL_LIBS})
      target_compile_definitions(runtime_loader_lock_tests PRIVATE
        CT_TRACE_CONSTRUCTOR_LIBRARY="$<TARGET_FILE:ct_trace_constructor_library>")
      add_dependencies(runtime_loader_lock_tests ct_trace_constructor_library)
    endif()
    if(TARGET ct_instrument_runtime_asan)
      # A global object's destructor that names a call, with the runtime that AddressSanitizer
      # instruments: a read of an object of the runtime destroyed before it fails (#206).
      add_executable(runtime_exit_trace_check runtime_exit_trace_check.cpp)
      target_include_directories(runtime_exit_trace_check PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
      target_link_libraries(runtime_exit_trace_check PRIVATE ct_instrument_runtime_asan
        coretrace_logger ${CMAKE_DL_LIBS})
      add_test(NAME RuntimeExitTrace COMMAND runtime_exit_trace_check)
    endif()
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
    # Fork handlers that call into the runtime while the forking thread holds its locks (#224),
    # in a process whose initialization registers them before the runtime registers its own.
    add_executable(runtime_fork_handlers_tests runtime_fork_handlers_test.cpp)
    target_include_directories(runtime_fork_handlers_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
    target_link_libraries(runtime_fork_handlers_tests PRIVATE ct_instrument_runtime coretrace_logger
      GTest::gtest_main)
  endif()
  if(WIN32)
    # A DLL replaced at its path by another build with the same time stamp (#208).
    target_sources(runtime_unit_tests PRIVATE runtime_vtable_replace_test.cpp)
    # A DLL that links its own copy of the runtime, which watches the loader too, and a DLL
    # whose constructor names a call with the loader lock held (#208).
    add_library(ct_runtime_library MODULE reload/ct_runtime_library.cpp)
    target_link_libraries(ct_runtime_library PRIVATE ct_instrument_runtime coretrace_logger)
    add_library(ct_dllmain_library MODULE reload/ct_dllmain_library.cpp)
    foreach(library ct_runtime_library ct_dllmain_library)
      coretrace_force_msvc_runtime(${library})
      add_dependencies(runtime_unit_tests ${library})
    endforeach()
    target_compile_definitions(runtime_unit_tests PRIVATE
      CT_RUNTIME_LIBRARY="$<TARGET_FILE:ct_runtime_library>"
      CT_DLLMAIN_LIBRARY="$<TARGET_FILE:ct_dllmain_library>")
    # A DLL's copy of the runtime reports its leaks when the process exits, after the DLL's
    # exit-time code, and says nothing when FreeLibrary unloads the DLL (#232). The program links
    # no runtime, so the report it prints is the DLL's copy's.
    add_library(ct_leak_dll MODULE reload/ct_leak_dll.cpp)
    target_link_libraries(ct_leak_dll PRIVATE ct_instrument_runtime coretrace_logger)
    add_executable(dll_leak_report_main dll_leak_report_main.cpp)
    target_compile_definitions(dll_leak_report_main PRIVATE
      CT_LEAK_DLL="$<TARGET_FILE:ct_leak_dll>")
    add_dependencies(dll_leak_report_main ct_leak_dll)
    foreach(target ct_leak_dll dll_leak_report_main)
      coretrace_force_msvc_runtime(${target})
    endforeach()
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
if(TARGET runtime_fork_modules_tests)
  gtest_discover_tests(runtime_fork_modules_tests)
endif()
if(TARGET runtime_loader_lock_tests)
  gtest_discover_tests(runtime_loader_lock_tests)
endif()
if(TARGET runtime_vtable_symbol_tests)
  gtest_discover_tests(runtime_vtable_symbol_tests)
endif()
if(TARGET runtime_fork_handlers_tests)
  gtest_discover_tests(runtime_fork_handlers_tests)
endif()
if(TARGET runtime_scan_threads_tests)
  gtest_discover_tests(runtime_scan_threads_tests PROPERTIES ENVIRONMENT CT_AUTOFREE_SCAN=1)
endif()
if(TARGET runtime_log_fallback_tests)
  # A limit outside the test: a writer that blocks also blocks the test's future in its
  # destructor, past the test's own wait.
  gtest_discover_tests(runtime_log_fallback_tests PROPERTIES TIMEOUT 120)
endif()
