cmake_minimum_required(VERSION 3.20)
project(HydrogenVM CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "HydrogenVM requires Clang or GCC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB_RECURSE HVM_RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp)
add_library(hvm_core STATIC
  src/core/memory/GuestArena.cpp
  src/core/cpu/Engine.cpp
  src/core/cpu/DecodeArm.cpp
  src/core/cpu/DecodeThumb.cpp
  src/core/cpu/DecodeVfpNeon.cpp
  src/core/cpu/DecodeNeon.cpp
  src/loader/ElfLoader.cpp
  src/loader/MachO.cpp
  src/loader/Zip.cpp
  src/loader/Plist.cpp
  src/loader/AppBundle.cpp
  src/selftest/SelfTest.cpp
  ${HVM_RUNTIME_SOURCES}
)
target_include_directories(hvm_core PUBLIC src)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
target_link_libraries(hvm_core PUBLIC ZLIB::ZLIB SQLite::SQLite3 Threads::Threads)
# -ffp-contract=off: guest VMLA/VMLS are unfused; never let the host compiler fuse them.
target_compile_options(hvm_core PRIVATE -Wall -Wextra -Wno-unused-parameter -foptimize-sibling-calls -fno-strict-aliasing -ffp-contract=off)

add_executable(hvm_tests tests/runner/TestRunner.cpp)
target_link_libraries(hvm_tests PRIVATE hvm_core)

add_executable(hvm_decodecheck tests/runner/DecodeCheck.cpp)
target_link_libraries(hvm_decodecheck PRIVATE hvm_core)

add_executable(hvm_heap_tests tests/runner/HeapTests.cpp)
target_link_libraries(hvm_heap_tests PRIVATE hvm_core)

add_executable(hvm_machodump tests/runner/MachODump.cpp)
target_link_libraries(hvm_machodump PRIVATE hvm_core)

add_executable(hvm tests/runner/HvmCli.cpp tests/runner/HvmRun.cpp)
target_link_libraries(hvm PRIVATE hvm_core)

add_executable(hvm_bench tests/runner/Bench.cpp)
target_link_libraries(hvm_bench PRIVATE hvm_core)
