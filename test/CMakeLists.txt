set(BINARY ${CMAKE_PROJECT_NAME}_test)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false *.h *.cpp)
# EmutlsShim.c provides a weak __emutls_get_address for the JIT to
# resolve via the process-symbol generator. See the file's header for
# the full rationale. Pulled in via an explicit .c glob since the
# default glob above only matches .h / .cpp.
file(GLOB_RECURSE TEST_C_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false *.c)
list(APPEND TEST_SOURCES ${TEST_C_SOURCES})
message(STATUS "TEST_SOURCES: ${TEST_SOURCES}")

cmake_policy(SET CMP0135 NEW)
cmake_policy(SET CMP0075 NEW)

find_package(GTest REQUIRED)

# Re-discover LLVM here purely to read LLVM_ENABLE_RTTI (find_package
# variables don't cross sibling add_subdirectory scopes; the real LLVM
# detection + libs come from src/). QUIET because src/ already reported it.
find_package(LLVM CONFIG QUIET)

add_executable(${BINARY} ${TEST_SOURCES})

# The networking test suite (Phase NET-*) shares fixtures/harnesses under
# test/net/ that sibling directories include as `"net/Foo.h"` (e.g.
# test/expression/NetCancellationTests.cpp -> "net/ReactorHarness.h"). Put the
# test root on the include path so those root-relative includes resolve from
# any subdirectory. Existing `"../jit/..."`-style relative includes are
# unaffected (they resolve relative to the including file).
target_include_directories(${BINARY} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ROCm / upstream LLVM is built with LLVM_ENABLE_RTTI=OFF, so it ships no
# C++ typeinfo for its own classes — it uses LLVM's hand-rolled
# isa<>/dyn_cast<> RTTI instead. The llvm::Error consumers (toString /
# cantFail / consumeError) expand inline to code that instantiates the
# C++ typeinfo for llvm::ErrorInfo<ErrorList, ErrorInfoBase>; compiled
# with the default -frtti that typeinfo dangles a reference to
# `typeinfo for llvm::ErrorInfoBase`, which the -fno-rtti LLVM omits ->
# undefined reference at link. Those calls are isolated in JitErrorShim.cpp
# (ANTLR-free, so unlike JitTestHelper.cpp it can be built -fno-rtti);
# compiling just that TU -fno-rtti emits no such typeinfo. The rest of
# test/ keeps RTTI (it dynamic_casts on cajeta + ANTLR types). Against a
# distro LLVM built with RTTI on, the symbol exists, so this is a no-op.
if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
    set_source_files_properties(jit/JitErrorShim.cpp PROPERTIES
            COMPILE_OPTIONS "-fno-rtti")
endif()

target_link_libraries(${BINARY} PRIVATE
        ${CMAKE_PROJECT_NAME}_lib
        GTest::gtest
        GTest::gtest_main)

# Export dynamic symbols so the runtime's SIGSEGV/SIGABRT backtrace handler
# (__cajeta_segv_handler -> backtrace_symbols_fd) can resolve NATIVE runtime
# frames to names at crash time. Without this, a crash on a CI runner we can't
# attach to (e.g. the aarch64 leg's parallel-stream SIGSEGVs) prints only
# `binary(+0xoffset)` — useless without the exact binary to addr2line. JIT'd
# frames still won't symbolize (not in the binary's symbol table), but the
# native scheduler/runtime frames — the suspects — will. POSIX only.
if(NOT WIN32)
    set_target_properties(${BINARY} PROPERTIES ENABLE_EXPORTS ON)
    target_link_options(${BINARY} PRIVATE -rdynamic)
endif()

# The build-tool HTTP test server (test/buildtool/TestHttpServer.h) speaks
# Winsock2 on Windows; link the sockets library.
if(WIN32)
    target_link_libraries(${BINARY} PRIVATE ws2_32)
endif()

# Bake the project source root into the test binary so the disk-reading
# CompilerTests (`canParseOnValidShortPackage`, etc.) can find their .cajeta
# fixtures without requiring CAJETA_SOURCE_ROOT to be set in the environment.
# Tests still honor the env var when present (overrides the default).
target_compile_definitions(${BINARY} PRIVATE
        CAJETA_SOURCE_ROOT_DEFAULT="${PROJECT_SOURCE_DIR}")

include(GoogleTest)
# Per-test timeout: individual tests should finish in well under 10s
# (JIT compile + run); 120s catches a hang (e.g. async / spawn deadlock
# under -j$(nproc) contention) without false-positiving on slow tests.
gtest_discover_tests(${BINARY}
        PROPERTIES TIMEOUT 120)

# `release_tests` target — runs the curated cross-compilation subset (see
# release_filter.txt + release_tests.sh) instead of the full battery. It
# delegates to the platform runner script, which reads release_filter.txt at
# run time (so the suite list stays a single source of truth) and shards the
# run. DEPENDS on the test binary so `cmake --build build --target
# release_tests` builds it first, then NO_BUILD avoids a redundant rebuild
# inside the script.
if(WIN32)
    add_custom_target(release_tests
            COMMAND ${CMAKE_COMMAND} -E env NO_BUILD=1
                    cmd /c "${PROJECT_SOURCE_DIR}/release_tests.cmd"
            DEPENDS ${BINARY}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            USES_TERMINAL
            COMMENT "Running cross-compilation release test subset")
else()
    add_custom_target(release_tests
            COMMAND ${CMAKE_COMMAND} -E env NO_BUILD=1
                    "${PROJECT_SOURCE_DIR}/release_tests.sh"
            DEPENDS ${BINARY}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            USES_TERMINAL
            COMMENT "Running cross-compilation release test subset")
endif()
