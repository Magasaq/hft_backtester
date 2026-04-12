cmake_minimum_required(VERSION 3.16)
project(hft_backtest VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ─────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -O3 -march=native
        -ffast-math
    )
endif()

# ── Include directories ────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/strategies)

# ── Main executable ────────────────────────────────────────────────────────
add_executable(hft_backtest main.cpp)

# Link filesystem library where needed
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(hft_backtest stdc++fs)
    target_link_libraries(hft_lob      stdc++fs)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    target_link_libraries(hft_backtest c++fs)
    target_link_libraries(hft_lob      c++fs)
endif()

# ── Data gen utility ───────────────────────────────────────────────────────
add_executable(gen_data tools/gen_data.cpp)

# ── Unit tests ────────────────────────────────────────────────────────────
add_executable(run_tests tests/test_main.cpp)

# ── Install ────────────────────────────────────────────────────────────────
install(TARGETS hft_backtest DESTINATION bin)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")

# ── LOB + trades mode ──────────────────────────────────────────────────────
add_executable(hft_lob main_lob.cpp)
