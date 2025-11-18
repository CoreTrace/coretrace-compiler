cmake_minimum_required(VERSION 3.16)
project(consumer_example CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  cc
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-compiler.git
  GIT_TAG main
)

FetchContent_MakeAvailable(cc)

add_executable(cc1 src/main.cpp)

target_link_libraries(cc1 PRIVATE cc::compilerlib_static)
# OR
# target_link_libraries(cc1 PRIVATE compilerlib_static)
