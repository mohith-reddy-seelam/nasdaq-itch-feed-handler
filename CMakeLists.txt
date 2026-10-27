cmake_minimum_required(VERSION 3.25)  # first CMake that knows CMAKE_CXX_STANDARD 26
project(itch_feed_handler VERSION 1.0.0 LANGUAGES CXX)

# C++26: static reflection, expansion statements, contracts, = delete("why"),
# plus C++23 std::start_lifetime_as for zero-copy message overlays.
# Needs GCC 16 or newer.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message(FATAL_ERROR "needs GCC 16+ for C++26 reflection and contracts "
                      "(found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

option(ITCH_NATIVE "Tune for the build machine (-march=native)" ON)
set(ITCH_SANITIZE "" CACHE STRING "Sanitizer to enable: address, thread, undefined, or empty")

# ---- header-only core: ITCH decoder, order book, pipeline -----------------------
add_library(itch INTERFACE)
target_include_directories(itch INTERFACE include third_party)
target_compile_features(itch INTERFACE cxx_std_26)

add_library(itch_flags INTERFACE)
target_compile_options(itch_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow
  -freflection                     # C++26 static reflection (GCC 16 keeps it behind a flag)
  -g -fno-omit-frame-pointer)
# GCC 16 ships the contract-violation handler and <debugging> in libstdc++exp.
target_link_libraries(itch_flags INTERFACE stdc++exp)
if(ITCH_NATIVE)
  target_compile_options(itch_flags INTERFACE -march=native)
endif()
if(ITCH_SANITIZE)
  target_compile_options(itch_flags INTERFACE -fsanitize=${ITCH_SANITIZE})
  target_link_options(itch_flags INTERFACE -fsanitize=${ITCH_SANITIZE})
endif()
if(MINGW)
  target_link_options(itch_flags INTERFACE -static)
endif()

# Profile-guided optimisation, two passes in the same build directory:
#   cmake --preset release -DITCH_PGO=generate && build && run a training replay
#   cmake --preset release -DITCH_PGO=use      && build   (scripts/pgo_build.sh)
set(ITCH_PGO "" CACHE STRING "Profile-guided optimisation: generate, use, or empty")
set(ITCH_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-profile" CACHE PATH "Where PGO profiles live")
if(ITCH_PGO STREQUAL "generate")
  target_compile_options(itch_flags INTERFACE -fprofile-generate=${ITCH_PGO_DIR} -fprofile-update=atomic)
  target_link_options(itch_flags INTERFACE -fprofile-generate=${ITCH_PGO_DIR})
elseif(ITCH_PGO STREQUAL "use")
  target_compile_options(itch_flags INTERFACE -fprofile-use=${ITCH_PGO_DIR}
    -fprofile-partial-training -Wno-missing-profile)
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
set(_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}} -std=c++26 -freflection")
if(ITCH_NATIVE)
  string(APPEND _flags " -march=native")
endif()
string(STRIP "${_flags}" _flags)

# ---- harness (vendored from spsc-ring-buffer) + mapped files --------------------
find_package(Threads REQUIRED)
add_library(itch_harness STATIC harness/platform.cpp harness/latency.cpp harness/mapped_file.cpp)
target_include_directories(itch_harness PUBLIC harness)
target_link_libraries(itch_harness PUBLIC itch itch_flags Threads::Threads)
target_compile_definitions(itch_harness PRIVATE
  SPSC_BUILD_FLAGS="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${_flags}")

# ---- applications ------------------------------------------------------------------
add_executable(itch_replay apps/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE itch_harness)

add_executable(itch_gen tools/itch_gen.cpp)
target_link_libraries(itch_gen PRIVATE itch itch_flags)

# Contracts are compiled out of everything that is measured.
foreach(t itch_harness itch_replay)
  target_compile_options(${t} PRIVATE -fcontract-evaluation-semantic=ignore)
endforeach()

# ---- dependencies ------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(benchmark googletest)

# ---- benchmarks ----------------------------------------------------------------------
add_executable(itch_bench bench/bench_main.cpp)
target_link_libraries(itch_bench PRIVATE itch_harness benchmark::benchmark)
target_compile_options(itch_bench PRIVATE -fcontract-evaluation-semantic=ignore)

# ---- tests ---------------------------------------------------------------------------
enable_testing()
add_executable(itch_tests
  tests/test_wire.cpp
  tests/test_mold.cpp
  tests/test_order_map.cpp
  tests/test_book.cpp
  tests/test_differential.cpp
  tests/test_pipeline.cpp)
target_link_libraries(itch_tests PRIVATE itch_harness GTest::gtest_main)
target_compile_options(itch_tests PRIVATE -fcontract-evaluation-semantic=enforce)
include(GoogleTest)
gtest_discover_tests(itch_tests DISCOVERY_TIMEOUT 60)
