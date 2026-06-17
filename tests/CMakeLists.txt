# GoogleTest is pulled in via FetchContent so the suite builds with nothing more
# than a compiler + CMake (no system-installed gtest needed). This keeps the
# Docker image and CI runner minimal and the build fully reproducible.
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# Don't override the parent project's compiler/linker settings on Windows; a
# no-op on this project's Linux/macOS targets but the recommended incantation.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(OBE_TEST_SOURCES
    test_price_time_priority.cpp
    test_partial_fills.cpp
    test_cancellation.cpp
    test_self_trade.cpp
    test_memory_pool.cpp
    test_ring_buffer.cpp
    test_risk_controls.cpp
    test_stress.cpp)

add_executable(obe_tests ${OBE_TEST_SOURCES})
target_link_libraries(obe_tests PRIVATE obe GTest::gtest_main)
target_compile_options(obe_tests PRIVATE -Wall -Wextra)

# Inherit sanitizers from the obe target in Debug builds (it propagates the
# flags via PUBLIC), so `ctest` on a Debug tree runs under ASan/UBSan.

include(GoogleTest)
gtest_discover_tests(obe_tests)
