cmake_minimum_required(VERSION 3.16)
project(cc)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

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
  src/compilerlib/toolchain.cpp
  src/compilerlib/instrumentation/alloc.cpp
  src/compilerlib/instrumentation/bounds.cpp
  src/compilerlib/instrumentation/common.cpp
  src/compilerlib/instrumentation/config.cpp
  src/compilerlib/instrumentation/trace.cpp
  src/compilerlib/instrumentation/vtable.cpp
)

add_library(ct_instrument_runtime STATIC
  src/runtime/ct_instrument_runtime.cpp
  src/runtime/ct_runtime_alloc.cpp
  src/runtime/ct_runtime_backtrace.cpp
  src/runtime/ct_runtime_bounds.cpp
  src/runtime/ct_runtime_env.cpp
  src/runtime/ct_runtime_logging.cpp
  src/runtime/ct_runtime_shadow.cpp
  src/runtime/ct_runtime_state.cpp
  src/runtime/ct_runtime_trace.cpp
  src/runtime/ct_runtime_vtable.cpp
)
set_target_properties(ct_instrument_runtime PROPERTIES OUTPUT_NAME "ct_instrument_runtime")
if(ENABLE_DEBUG_ASAN)
  target_compile_options(ct_instrument_runtime PRIVATE -fno-sanitize=address)
endif()

add_library(compilerlib_static STATIC ${LIB_SOURCES})

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

message(STATUS "Using Clang resource dir: ${CLANG_RESOURCE_DIR}")

# FOR USE WITH FETCHCONTENT
target_include_directories(compilerlib_static
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# ALIAS FOR USE WITH FETCHCONTENT
add_library(cc::compilerlib_static ALIAS compilerlib_static)

target_compile_definitions(compilerlib_static
  PRIVATE
    CT_RUNTIME_LIB_PATH="$<TARGET_FILE:ct_instrument_runtime>"
)
add_dependencies(compilerlib_static ct_instrument_runtime)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(compilerlib_static PRIVATE -fno-rtti)
endif()

if(CLANG_LINK_CLANG_DYLIB)
  target_link_libraries(compilerlib_static PUBLIC clang-cpp)
else()
  target_link_libraries(compilerlib_static PUBLIC
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

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(compilerlib_static PUBLIC LLVM)
else()
  target_link_libraries(compilerlib_static PUBLIC
  LLVMOption
  LLVMSupport
  LLVMTarget
    LLVMX86AsmParser
    LLVMX86CodeGen
    LLVMX86Desc
    LLVMX86Info
    LLVMObject
    LLVMBinaryFormat
  )
endif()

add_library(compilerlib_shared SHARED ${LIB_SOURCES})

set_target_properties(compilerlib_shared PROPERTIES OUTPUT_NAME "compilerlib")

# FOR USE WITH FETCHCONTENT
target_include_directories(compilerlib_shared
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# ALIAS FOR USE WITH FETCHCONTENT
add_library(cc::compilerlib_shared ALIAS compilerlib_shared)

target_compile_definitions(compilerlib_shared
  PRIVATE
    CT_RUNTIME_LIB_PATH="$<TARGET_FILE:ct_instrument_runtime>"
)
add_dependencies(compilerlib_shared ct_instrument_runtime)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(compilerlib_shared PRIVATE -fno-rtti)
endif()

if(CLANG_LINK_CLANG_DYLIB)
  target_link_libraries(compilerlib_shared PRIVATE clang-cpp)
else()
  target_link_libraries(compilerlib_shared PRIVATE clangAST clangBasic clangCodeGen clangDriver clangFrontend clangLex clangParse clangSema )
endif()

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(compilerlib_shared PRIVATE LLVM)
else()
  target_link_libraries(compilerlib_shared PRIVATE
  LLVMOption
  LLVMSupport
  LLVMTarget
  LLVMX86AsmParser
  LLVMX86CodeGen
  LLVMX86Desc
  LLVMX86Info
  LLVMObject
  LLVMBinaryFormat
  )
endif()

add_executable(cc src/cli/main.cc src/cli/help.cc src/cli/args.cc)
target_include_directories(cc PRIVATE src)

# Propagate CLANG_RESOURCE_DIR as a compile definition to all relevant targets
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

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cc PRIVATE -fno-rtti)
endif()

target_link_libraries(cc PRIVATE compilerlib_static)

if(CLANG_LINK_CLANG_DYLIB)
  target_link_libraries(cc PRIVATE clang-cpp)
else()
  target_link_libraries(cc PRIVATE
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

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(cc PRIVATE LLVM)
else()
  target_link_libraries(cc PRIVATE
    LLVMOption
    LLVMSupport
    LLVMTarget
    LLVMX86AsmParser
    LLVMX86CodeGen
    LLVMX86Desc
    LLVMX86Info
    LLVMObject
    LLVMBinaryFormat
  )
endif()

install(TARGETS compilerlib_static compilerlib_shared cc ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

set(USE_SHARED_LIB OFF CACHE BOOL "Link with shared compilerlib")

if(BUILD_TESTS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/test_compiler.cpp")
    add_executable(test_compiler test/test_compiler.cpp)

    if(NOT LLVM_ENABLE_RTTI)
      target_compile_options(test_compiler PRIVATE -fno-rtti)
    endif()

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

# ============
#  FORMATTING
# ============
# Only expose formatting targets when this project is the top-level build to
# avoid name clashes when used via FetchContent.
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
