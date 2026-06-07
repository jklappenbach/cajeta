# Local TDD harness for the debugger / in-process JIT host work.
#
# Deliberately SEPARATE from the main `cajeta_test` battery (test/) and from
# the project's cajeta_tests flow — this is the debugger sub-project's own test
# target. It is NOT registered with ctest (no gtest_discover_tests), so it does
# not fold into the larger suite. Build and run it directly:
#
#   cmake --build build --target cajeta_debug_test
#   ./build/debug-tests/cajeta_debug_test            # all
#   ./build/debug-tests/cajeta_debug_test --gtest_filter=JitHost.*
#
set(BINARY cajeta_debug_test)

# find_package results don't cross sibling add_subdirectory scopes; re-find here.
find_package(GTest REQUIRED)

file(GLOB_RECURSE DEBUG_TEST_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false
        *.cpp *.h)

add_executable(${BINARY} ${DEBUG_TEST_SOURCES})

# Own main() (debug-tests/main.cpp) instead of GTest::gtest_main — it _exit()s
# with the gtest status to skip a fragile LLVM/runtime static-destructor
# teardown after many in-process JIT runs (which can exit non-zero after every
# test has passed). See debug-tests/main.cpp.
target_link_libraries(${BINARY} PRIVATE
        ${CMAKE_PROJECT_NAME}_lib
        GTest::gtest)

target_include_directories(${BINARY} PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Bake the project source root so tests can locate the in-tree sample fixtures
# (samples/jit-smoke/...) without an env var.
target_compile_definitions(${BINARY} PRIVATE
        CAJETA_SOURCE_ROOT_DEFAULT="${PROJECT_SOURCE_DIR}")

# Intentionally NO gtest_discover_tests: keeps these out of the main ctest
# suite. Run the binary directly for the debugger TDD loop.
