# HydrogenVM for Android: the emulator core (the same sources the desktop and iPhone builds use)
# plus the JNI bridge the Android app talks to (hvm_android.cpp).
cmake_minimum_required(VERSION 3.22)
project(hydrogenvm_android C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(HVM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# SQLite is not part of the NDK: the official amalgamation, pinned by hash.
include(FetchContent)
FetchContent_Declare(sqlite3_amalgamation
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784)
FetchContent_MakeAvailable(sqlite3_amalgamation)
add_library(hvm_sqlite3 STATIC ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(hvm_sqlite3 PUBLIC ${sqlite3_amalgamation_SOURCE_DIR})
target_compile_definitions(hvm_sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)

# Keep this list in step with hvm_core in the top-level CMakeLists.txt.
file(GLOB_RECURSE HVM_RUNTIME_SOURCES CONFIGURE_DEPENDS ${HVM_ROOT}/src/runtime/*.cpp)
add_library(hvm_core STATIC
  ${HVM_ROOT}/src/core/memory/GuestArena.cpp
  ${HVM_ROOT}/src/core/cpu/Engine.cpp
  ${HVM_ROOT}/src/core/cpu/BlockCache.cpp
  ${HVM_ROOT}/src/core/cpu/QuickSave.cpp
  ${HVM_ROOT}/src/core/cpu/DecodeArm.cpp
  ${HVM_ROOT}/src/core/cpu/DecodeThumb.cpp
  ${HVM_ROOT}/src/core/cpu/DecodeVfpNeon.cpp
  ${HVM_ROOT}/src/core/cpu/DecodeNeon.cpp
  ${HVM_ROOT}/src/loader/ElfLoader.cpp
  ${HVM_ROOT}/src/loader/MachO.cpp
  ${HVM_ROOT}/src/loader/Zip.cpp
  ${HVM_ROOT}/src/loader/Apk.cpp
  ${HVM_ROOT}/src/loader/Plist.cpp
  ${HVM_ROOT}/src/loader/AppBundle.cpp
  ${HVM_ROOT}/src/loader/AndroidEngine.cpp
  ${HVM_ROOT}/src/selftest/SelfTest.cpp
  ${HVM_RUNTIME_SOURCES})
target_include_directories(hvm_core PUBLIC ${HVM_ROOT}/src)
target_compile_definitions(hvm_core PRIVATE HVM_COMMIT="android")
target_compile_options(hvm_core PRIVATE -Wno-unused-parameter -fno-strict-aliasing -ffp-contract=off -foptimize-sibling-calls)
# libicu.so is part of the NDK from Android 12 (API 31); zlib, EGL, GLES and AAudio always.
target_link_libraries(hvm_core PUBLIC hvm_sqlite3 z icu log)

add_library(hydrogenvm SHARED hvm_android.cpp)
target_link_libraries(hydrogenvm PRIVATE hvm_core android EGL GLESv2 aaudio log)
