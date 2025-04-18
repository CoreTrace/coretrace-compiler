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

# Option pour activer ou désactiver les tests
option(BUILD_TESTS "Build test executables" OFF)

set(LIB_SOURCES src/compilerlib/compiler.cpp)

add_library(compilerlib_static STATIC ${LIB_SOURCES})
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(compilerlib_static PRIVATE -fno-rtti)
endif()

if(CLANG_LINK_CLANG_DYLIB)
  target_link_libraries(compilerlib_static PRIVATE clang-cpp)
else()
  target_link_libraries(compilerlib_static PRIVATE
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
  target_link_libraries(compilerlib_static PRIVATE LLVM)
else()
  target_link_libraries(compilerlib_static PRIVATE LLVMOption LLVMSupport LLVMTarget LLVMX86AsmParser LLVMX86CodeGen LLVMX86Desc LLVMX86Info )
endif()

add_library(compilerlib_shared SHARED ${LIB_SOURCES})
set_target_properties(compilerlib_shared PROPERTIES OUTPUT_NAME "compilerlib")

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
  )
endif()

add_executable(cc src/cli/main.cc)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cc PRIVATE -fno-rtti)
endif()

target_link_libraries(cc PRIVATE compilerlib_static)

install(TARGETS compilerlib_static compilerlib_shared cc ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

set(USE_SHARED_LIB OFF CACHE BOOL "Link with shared compilerlib")

# Construction conditionnelle des tests
if(BUILD_TESTS)
  # Vérifier si le fichier de test existe
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
