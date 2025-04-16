cmake_minimum_required(VERSION 3.16)
project(cc)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

add_executable(cc main.cc)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(cc PRIVATE -fno-rtti)
endif()

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
  target_link_libraries(cc PRIVATE LLVMOption LLVMSupport LLVMTarget
    LLVMX86AsmParser LLVMX86CodeGen LLVMX86Desc LLVMX86Info)
endif()
