cmake_minimum_required(VERSION 3.20)
project(HydrogenVM CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "HydrogenVM requires clang (uses [[clang::musttail]]). Configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(hvm_core STATIC
  src/core/memory/GuestArena.cpp
  src/core/cpu/Engine.cpp
  src/core/cpu/DecodeArm.cpp
  src/core/cpu/DecodeThumb.cpp
  src/core/cpu/DecodeVfpNeon.cpp
  src/loader/ElfLoader.cpp
)
target_include_directories(hvm_core PUBLIC src)
target_compile_options(hvm_core PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)

add_executable(hvm_tests tests/runner/TestRunner.cpp)
target_link_libraries(hvm_tests PRIVATE hvm_core)

add_executable(hvm_bench tests/runner/Bench.cpp)
target_link_libraries(hvm_bench PRIVATE hvm_core)
