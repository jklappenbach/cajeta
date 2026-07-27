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

# Export the binary's symbols dynamically, exactly as test/CMakeLists.txt does.
# The in-process JIT resolves the runtime's NATIVE symbols out of the host
# process's dynamic symbol table; without -rdynamic that table is empty, so
# every JIT-running test here died with
#   JIT session error: Symbols not found: [ __cajeta_tls_*, cajeta_xpu_optix_* ]
# — which is why JitHost / DebugSession / SafepointCodegen / FiberView all failed
# in this target while passing in cajeta_test. Nothing runs this binary in the
# normal flow (it is not in ctest, and cajeta_tests.sh does not cover it), so the
# gap went unnoticed. POSIX only.
#
# `-u __cajeta_tls_ctx_new` on top: the TLS engine (runtime/native/cajeta_tls.c)
# is a standalone object inside libcajeta_lib.a, and a static archive only yields
# the objects something REFERENCES. cajeta_test has net tests that call it, so it
# gets pulled in there; nothing here does, so it was absent from this binary and
# the JIT could not resolve the stdlib's TlsConnection @Native thunks — which are
# always linked (eager clinit ctors root every class, so --gc-sections cannot drop
# the unused net subsystem). One forced undefined reference pulls the object in,
# and with it every __cajeta_tls_* symbol.
#
# `-u` takes a LINKER-level symbol name, and Mach-O prefixes every C symbol with
# an underscore that ELF does not. The C identifier is `__cajeta_tls_ctx_new`, so
# the name to force is `__cajeta_tls_ctx_new` on ELF but `___cajeta_tls_ctx_new`
# (three) on Apple — passing the ELF spelling there fails the link outright with
# "Undefined symbols ... __cajeta_tls_ctx_new, referenced from <initial-undefines>".
if(NOT WIN32)
    set_target_properties(${BINARY} PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(${BINARY} PRIVATE -rdynamic -Wl,-u,___cajeta_tls_ctx_new)
    else()
        target_link_options(${BINARY} PRIVATE -rdynamic -Wl,-u,__cajeta_tls_ctx_new)
    endif()
endif()

# Same COMDAT-placement mismatch cajeta_test hits on MinGW: the prebuilt LLVM/lld
# archives come from a different GCC than the runner's MSYS2 g++, so a shared
# libstdc++ inline template's function-local statics can land in different
# sections and ld calls it a multiple definition rather than folding it. See the
# longer note in test/CMakeLists.txt.
if(MINGW)
    target_link_options(${BINARY} PRIVATE -Wl,--allow-multiple-definition)
endif()

# Bake the project source root so tests can locate the in-tree sample fixtures
# (samples/jit-smoke/...) without an env var.
target_compile_definitions(${BINARY} PRIVATE
        CAJETA_SOURCE_ROOT_DEFAULT="${PROJECT_SOURCE_DIR}")

# Intentionally NO gtest_discover_tests: keeps these out of the main ctest
# suite. Run the binary directly for the debugger TDD loop.
