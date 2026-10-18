cmake_minimum_required(VERSION 3.20)
project(ArgonVM CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "ArgonVM requires clang (uses [[clang::musttail]]). Configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(argon_core STATIC
  src/core/memory/GuestArena.cpp
  src/core/cpu/Engine.cpp
  src/core/cpu/DecodeArm.cpp
  src/core/cpu/DecodeThumb.cpp
  src/core/cpu/DecodeVfpNeon.cpp
  src/loader/ElfLoader.cpp
)
target_include_directories(argon_core PUBLIC src)
target_compile_options(argon_core PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)

add_executable(argon_tests tests/runner/TestRunner.cpp)
target_link_libraries(argon_tests PRIVATE argon_core)

add_executable(argon_bench tests/runner/Bench.cpp)
target_link_libraries(argon_bench PRIVATE argon_core)
