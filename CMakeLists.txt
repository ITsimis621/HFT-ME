cmake_minimum_required(VERSION 3.15)
project(hft_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict HFT compiler optimizations
# -O3 & -flto: Maximum inlining and interprocedural optimization.
# -march=native: Unlocks hardware-specific instructions (like SSE4.2 _mm_crc32_u64).
# -fno-exceptions & -fno-rtti: Eliminates zero-cost exception overhead and RTTI bloat, ensuring deterministic hot paths.
add_compile_options(
    -O3
    -march=native
    -flto
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Werror
)
add_link_options(-flto)

find_package(Threads REQUIRED)

# Shared include paths matching the modular directory layout
set(ENGINE_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
)

# Main Engine Executable
add_executable(hft_engine src/main.cpp)
target_link_libraries(hft_engine PRIVATE Threads::Threads)
target_include_directories(hft_engine PRIVATE ${ENGINE_INCLUDES})

# Unit & Invariant Test Executable
enable_testing()
add_executable(test_lob tests/test_lob.cpp)
target_link_libraries(test_lob PRIVATE Threads::Threads)
target_include_directories(test_lob PRIVATE ${ENGINE_INCLUDES})
add_test(NAME LimitOrderBookTests COMMAND test_lob)