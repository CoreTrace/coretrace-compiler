# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
project(cc)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(coretrace_force_msvc_runtime target_name)
  if(MSVC AND TARGET "${target_name}")
    set_property(TARGET "${target_name}" PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
  endif()
endfunction()

function(coretrace_disable_rtti target_name)
  if(NOT TARGET "${target_name}" OR LLVM_ENABLE_RTTI)
    return()
  endif()

  if(MSVC)
    target_compile_options("${target_name}" PRIVATE /GR-)
  else()
    target_compile_options("${target_name}" PRIVATE -fno-rtti)
  endif()
endfunction()

function(coretrace_patch_imported_link_property target_name property_name stale_value replacement_value)
  if(NOT TARGET "${target_name}")
    return()
  endif()

  get_target_property(current_value "${target_name}" "${property_name}")
  if(NOT current_value OR current_value STREQUAL "current_value-NOTFOUND")
    return()
  endif()

  string(REPLACE "${stale_value}" "${replacement_value}" updated_value "${current_value}")
  if(NOT updated_value STREQUAL current_value)
    set_target_properties("${target_name}" PROPERTIES
      ${property_name} "${updated_value}"
    )
  endif()
endfunction()

function(coretrace_patch_llvm_diaguids replacement_path)
  if(NOT WIN32 OR NOT TARGET LLVMDebugInfoPDB)
    return()
  endif()

  set(coretrace_stale_diaguids_path
      "C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional/DIA SDK/lib/amd64/diaguids.lib")

  foreach(property_name
          INTERFACE_LINK_LIBRARIES
          IMPORTED_LINK_INTERFACE_LIBRARIES
          IMPORTED_LINK_INTERFACE_LIBRARIES_DEBUG
          IMPORTED_LINK_INTERFACE_LIBRARIES_RELEASE
          IMPORTED_LINK_INTERFACE_LIBRARIES_RELWITHDEBINFO
          IMPORTED_LINK_INTERFACE_LIBRARIES_MINSIZEREL)
    coretrace_patch_imported_link_property(
      LLVMDebugInfoPDB
      ${property_name}
      "${coretrace_stale_diaguids_path}"
      "${replacement_path}"
    )
  endforeach()
endfunction()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

if(WIN32)
  set(coretrace_diaguids_candidates)
  file(GLOB_RECURSE coretrace_diaguids_candidates
    LIST_DIRECTORIES false
    "C:/Program Files/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64/diaguids.lib"
    "C:/Program Files (x86)/Microsoft Visual Studio/*/*/DIA SDK/lib/amd64/diaguids.lib"
  )

  if(coretrace_diaguids_candidates)
    list(GET coretrace_diaguids_candidates 0 coretrace_diaguids_path)
    coretrace_patch_llvm_diaguids("${coretrace_diaguids_path}")
  endif()
endif()

include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
  include
)

option(BUILD_TESTS "Build test executables" OFF)
option(ENABLE_DEBUG_ASAN "Enable debug symbols and AddressSanitizer" OFF)

if(ENABLE_DEBUG_ASAN)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set(ASAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    add_compile_options(-g -O0 ${ASAN_FLAGS})
    add_link_options(${ASAN_FLAGS})
  else()
    message(WARNING "ENABLE_DEBUG_ASAN is ON but ASAN flags are only set for Clang/GNU compilers.")
  endif()
endif()

set(LIB_SOURCES
  src/compilerlib/compiler.cpp
  src/compilerlib/emit/llvm_output.cpp
  src/compilerlib/frontend/optnone_action.cpp
  src/compilerlib/toolchain.cpp
  src/compilerlib/instrumentation/alloc.cpp
  src/compilerlib/instrumentation/bounds.cpp
  src/compilerlib/instrumentation/common.cpp
  src/compilerlib/instrumentation/config.cpp
  src/compilerlib/instrumentation/trace.cpp
  src/compilerlib/instrumentation/vtable.cpp
)

set(CT_RUNTIME_PLATFORM_SOURCES
  src/runtime/ct_runtime_alloc.cpp
  src/runtime/ct_runtime_backtrace.cpp
  src/runtime/ct_runtime_env.cpp
  src/runtime/ct_runtime_vtable.cpp
)
if(WIN32)
  set(CT_RUNTIME_PLATFORM_SOURCES
    src/runtime/windows/ct_runtime_alloc.cpp
    src/runtime/windows/ct_runtime_backtrace.cpp
    src/runtime/windows/ct_runtime_env.cpp
    src/runtime/windows/ct_runtime_vtable.cpp
  )
endif()

set(CORETRACE_LOGGER_BUILD_EXAMPLES OFF CACHE BOOL "Disable logger examples" FORCE)
set(CORETRACE_LOGGER_BUILD_TESTS OFF CACHE BOOL "Disable logger tests" FORCE)

include(FetchContent)
FetchContent_Declare(coretrace_logger
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-log.git
  GIT_TAG        main
  EXCLUDE_FROM_ALL
)

FetchContent_GetProperties(coretrace_logger)
if(NOT coretrace_logger_POPULATED)
  FetchContent_Populate(coretrace_logger)
endif()

if(NOT TARGET coretrace_logger)
  add_subdirectory("${coretrace_logger_SOURCE_DIR}" "${coretrace_logger_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
coretrace_force_msvc_runtime(coretrace_logger)

add_library(ct_instrument_runtime STATIC
  src/runtime/ct_instrument_runtime.cpp
  ${CT_RUNTIME_PLATFORM_SOURCES}
  src/runtime/ct_runtime_bounds.cpp
  src/runtime/ct_runtime_logging.cpp
  src/runtime/ct_runtime_shadow.cpp
  src/runtime/ct_runtime_state.cpp
  src/runtime/ct_runtime_trace.cpp
)
target_include_directories(ct_instrument_runtime PRIVATE src/runtime)
target_link_libraries(ct_instrument_runtime PRIVATE coretrace_logger)
if(WIN32)
  target_link_libraries(ct_instrument_runtime PRIVATE dbghelp)
endif()
set_target_properties(ct_instrument_runtime PROPERTIES OUTPUT_NAME "ct_instrument_runtime")
coretrace_force_msvc_runtime(ct_instrument_runtime)
if(ENABLE_DEBUG_ASAN)
  target_compile_options(ct_instrument_runtime PRIVATE -fno-sanitize=address)
endif()

add_library(compilerlib_static STATIC ${LIB_SOURCES})
coretrace_force_msvc_runtime(compilerlib_static)

# Generic detection of Clang resource directory.
# You can override this with -DCLANG_RESOURCE_DIR=/custom/path when configuring CMake.
set(CLANG_RESOURCE_DIR "" CACHE STRING "Path to Clang resource directory")

if(NOT CLANG_RESOURCE_DIR)
  set(_CLANG_VERSIONED_CANDIDATES
    clang-${LLVM_VERSION_MAJOR}
    clang++-${LLVM_VERSION_MAJOR}
  )
  set(_CLANG_GENERIC_CANDIDATES
    clang
    clang++
  )
  get_filename_component(_LLVM_PREFIX "${LLVM_DIR}/../../.." ABSOLUTE)
  set(_LLVM_BIN_DIR "${_LLVM_PREFIX}/bin")
  find_program(CLANG_EXECUTABLE
    NAMES ${_CLANG_VERSIONED_CANDIDATES} ${_CLANG_GENERIC_CANDIDATES}
    PATHS ${LLVM_TOOLS_BINARY_DIR} ${_LLVM_BIN_DIR}
    NO_DEFAULT_PATH
    DOC "Path to the clang executable used to detect resource dir"
  )
  if(NOT CLANG_EXECUTABLE)
    find_program(CLANG_EXECUTABLE
      NAMES ${_CLANG_VERSIONED_CANDIDATES}
      DOC "Path to the clang executable used to detect resource dir"
    )
  endif()

  if(NOT CLANG_EXECUTABLE)
    message(FATAL_ERROR "Unable to find clang executable. Please set CLANG_RESOURCE_DIR manually.")
  endif()

  execute_process(
    COMMAND ${CLANG_EXECUTABLE} --version
    OUTPUT_VARIABLE _CLANG_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  string(REGEX MATCH "(Apple clang|clang) version ([0-9]+)" _CLANG_VERSION_MATCH "${_CLANG_VERSION_OUTPUT}")
  if(NOT _CLANG_VERSION_MATCH)
    message(WARNING "Unable to parse clang version from: ${_CLANG_VERSION_OUTPUT}")
  elseif(NOT CMAKE_MATCH_2 STREQUAL "${LLVM_VERSION_MAJOR}")
    message(FATAL_ERROR "Clang version mismatch: found ${CMAKE_MATCH_2}, expected ${LLVM_VERSION_MAJOR}. Please set CLANG_RESOURCE_DIR or CLANG_EXECUTABLE explicitly.")
  endif()

  execute_process(
    COMMAND ${CLANG_EXECUTABLE} -print-resource-dir
    OUTPUT_VARIABLE CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

file(TO_CMAKE_PATH "${CLANG_RESOURCE_DIR}" CLANG_RESOURCE_DIR)
if(_LLVM_BIN_DIR)
  file(TO_CMAKE_PATH "${_LLVM_BIN_DIR}" _LLVM_BIN_DIR)
endif()
if(CLANG_EXECUTABLE)
  file(TO_CMAKE_PATH "${CLANG_EXECUTABLE}" CLANG_EXECUTABLE)
endif()

message(STATUS "Using Clang resource dir: ${CLANG_RESOURCE_DIR}")

if(LLVM_LINK_LLVM_DYLIB)
  set(CT_LLVM_LINK_LIBS LLVM)
else()
  llvm_map_components_to_libnames(CT_LLVM_LINK_LIBS
    all
    AllTargetsInfos
    AllTargetsCodeGens
    AllTargetsAsmParsers
    AllTargetsDescs
    AllTargetsMCAs
  )
endif()

if(CLANG_LINK_CLANG_DYLIB)
  set(CT_CLANG_LINK_LIBS clang-cpp)
else()
  set(CT_CLANG_LINK_LIBS
    clangAST
    clangBasic
    clangCodeGen
    clangDriver
    clangFrontend
    clangLex
    clangParse
    clangSema
  )
endif()

target_include_directories(compilerlib_static
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
add_library(cc::compilerlib_static ALIAS compilerlib_static)

target_compile_definitions(compilerlib_static
  PRIVATE
    CT_RUNTIME_LIB_PATH="$<TARGET_FILE:ct_instrument_runtime>"
    CT_RUNTIME_LOGGER_LIB_PATH="$<TARGET_FILE:coretrace_logger>"
)
add_dependencies(compilerlib_static ct_instrument_runtime)
coretrace_disable_rtti(compilerlib_static)
target_link_libraries(compilerlib_static PUBLIC ${CT_CLANG_LINK_LIBS} ${CT_LLVM_LINK_LIBS})

add_library(compilerlib_shared SHARED ${LIB_SOURCES})
set_target_properties(compilerlib_shared PROPERTIES OUTPUT_NAME "compilerlib")
coretrace_force_msvc_runtime(compilerlib_shared)

target_include_directories(compilerlib_shared
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
add_library(cc::compilerlib_shared ALIAS compilerlib_shared)

target_compile_definitions(compilerlib_shared
  PRIVATE
    CT_RUNTIME_LIB_PATH="$<TARGET_FILE:ct_instrument_runtime>"
    CT_RUNTIME_LOGGER_LIB_PATH="$<TARGET_FILE:coretrace_logger>"
)
add_dependencies(compilerlib_shared ct_instrument_runtime)
coretrace_disable_rtti(compilerlib_shared)
target_link_libraries(compilerlib_shared PRIVATE ${CT_CLANG_LINK_LIBS} ${CT_LLVM_LINK_LIBS})

add_executable(cc src/cli/main.cc src/cli/help.cc src/cli/args.cc)
target_include_directories(cc PRIVATE src)
coretrace_force_msvc_runtime(cc)

foreach(tgt compilerlib_static compilerlib_shared cc)
  if(TARGET ${tgt})
    target_compile_definitions(${tgt}
      PRIVATE
        CLANG_RESOURCE_DIR=\"${CLANG_RESOURCE_DIR}\"
    )
    if(_LLVM_BIN_DIR)
      target_compile_definitions(${tgt}
        PRIVATE
          CT_LLVM_BIN_DIR=\"${_LLVM_BIN_DIR}\"
      )
    endif()
    if(CLANG_EXECUTABLE)
      target_compile_definitions(${tgt}
        PRIVATE
          CT_CLANG_EXECUTABLE=\"${CLANG_EXECUTABLE}\"
      )
    endif()
  endif()
endforeach()

coretrace_disable_rtti(cc)
target_link_libraries(cc PRIVATE compilerlib_static ${CT_CLANG_LINK_LIBS} ${CT_LLVM_LINK_LIBS})

install(TARGETS compilerlib_static compilerlib_shared cc
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)

set(USE_SHARED_LIB OFF CACHE BOOL "Link with shared compilerlib")

if(BUILD_TESTS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/test_compiler.cpp")
    add_executable(test_compiler test/test_compiler.cpp)
    coretrace_force_msvc_runtime(test_compiler)
    coretrace_disable_rtti(test_compiler)

    if(USE_SHARED_LIB)
      target_link_libraries(test_compiler PRIVATE compilerlib_shared)
      set_target_properties(test_compiler PROPERTIES
        BUILD_RPATH "@loader_path/../lib"
        INSTALL_RPATH "@loader_path/../lib"
      )
    else()
      target_link_libraries(test_compiler PRIVATE compilerlib_static)
    endif()

    target_include_directories(test_compiler PRIVATE include)
  else()
    message(STATUS "Test file test/test_compiler.cpp not found, skipping test build")
  endif()
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  add_custom_target(format
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Run clang-format on source files")

  add_custom_target(format-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/format-check.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Verify clang-format compliance")
endif()
