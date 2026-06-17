cmake_minimum_required(VERSION 3.16)

project(order_book_engine
    VERSION 1.0.0
    DESCRIPTION "Low-latency limit order book matching engine"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd / tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if the user did not specify a build type. The two
# first-class configurations are:
#   -DCMAKE_BUILD_TYPE=Release  -> -O3, NDEBUG, LTO (the "production" build)
#   -DCMAKE_BUILD_TYPE=Debug    -> -O0 -g + ASan + UBSan (the "verify" build)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No build type specified; defaulting to Release.")
endif()

# ---------------------------------------------------------------------------
# Per-configuration flags
# ---------------------------------------------------------------------------
set(OBE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fno-omit-frame-pointer")

# AddressSanitizer + UndefinedBehaviorSanitizer are wired into the Debug
# configuration so that `cmake --build` of a Debug tree runs the full test
# suite under instrumentation.
set(OBE_SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all)

# ---------------------------------------------------------------------------
# Core library: the engine itself, free of any main()/test code.
# ---------------------------------------------------------------------------
add_library(obe
    src/Order.cpp
    src/PriceLevel.cpp
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/OrderGenerator.cpp
    src/RiskControls.cpp)

target_include_directories(obe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(obe PRIVATE ${OBE_WARNINGS})

find_package(Threads REQUIRED)
target_link_libraries(obe PUBLIC Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(obe PUBLIC ${OBE_SANITIZER_FLAGS})
    target_link_options(obe PUBLIC ${OBE_SANITIZER_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Benchmark harness
# ---------------------------------------------------------------------------
add_executable(obe_bench bench/benchmark.cpp)
target_link_libraries(obe_bench PRIVATE obe)
target_compile_options(obe_bench PRIVATE ${OBE_WARNINGS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
