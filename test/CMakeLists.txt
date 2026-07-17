set(BINARY ${CMAKE_PROJECT_NAME}_test)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false *.h *.cpp)
# EmutlsShim.c provides a weak __emutls_get_address for the JIT to
# resolve via the process-symbol generator. See the file's header for
# the full rationale. Pulled in via an explicit .c glob since the
# default glob above only matches .h / .cpp.
file(GLOB_RECURSE TEST_C_SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false *.c)
list(APPEND TEST_SOURCES ${TEST_C_SOURCES})
# spike_optix_ll.cpp is a SELF-CONTAINED standalone harness (its own main() + the
# OptiX function-table definition) for the M2 Phase-1 spike — built by hand, not part
# of the gtest binary (which would multiply-define main / g_optixFunctionTable). Keep
# it in-tree for reproducibility but exclude it from the auto-glob.
list(FILTER TEST_SOURCES EXCLUDE REGEX "xpu/optix/spike_optix_ll\\.cpp$")
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

# OptiX (NVIDIA RT-core ray-query) parity probe — header-only detection, mirroring
# the runtime's __has_include(<vulkan/vulkan.h>) gating idiom. OptiX is a COMPILE-TIME
# dependency only: the headers here, plus the driver's nvoptix.dll at runtime (loaded
# via optixInit, like cajeta dlopen's nvcuda.dll) — no OptiX library to link or ship.
# Absent SDK -> CAJETA_TEST_HAS_OPTIX undefined -> OptiXRayQueryProbeTests.cpp compiles
# to nothing (software floor unaffected). Override search with -DCAJETA_OPTIX_ROOT=<sdk>.
find_path(CAJETA_OPTIX_INCLUDE_DIR optix.h
        HINTS "${CAJETA_OPTIX_ROOT}/include" "$ENV{OptiX_INSTALL_DIR}/include"
              "$ENV{OPTIX_ROOT}/include"
        PATHS "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.1.0/include"
              "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.0.0/include"
              "/opt/optix/include"
        DOC "OptiX SDK include directory (contains optix.h)")
# OptiX headers #include <cuda.h>; locate the CUDA toolkit include dir for it.
find_path(CAJETA_CUDA_INCLUDE_DIR cuda.h
        HINTS "$ENV{CUDA_PATH}/include"
        PATHS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.9/include"
        DOC "CUDA toolkit include directory (cuda.h, required by the OptiX headers)")
if(CAJETA_OPTIX_INCLUDE_DIR AND CAJETA_CUDA_INCLUDE_DIR)
    message(STATUS "OptiX RT-core probe ENABLED: ${CAJETA_OPTIX_INCLUDE_DIR}")
    target_include_directories(${BINARY} PRIVATE
            ${CAJETA_OPTIX_INCLUDE_DIR} ${CAJETA_CUDA_INCLUDE_DIR})
    target_compile_definitions(${BINARY} PRIVATE CAJETA_TEST_HAS_OPTIX=1)
    if(WIN32)
        # The OptiX Windows DLL loader (optixLoadWindowsDllFromName) uses the
        # Configuration-Manager + registry APIs to locate nvoptix.dll.
        target_link_libraries(${BINARY} PRIVATE cfgmgr32 advapi32)
    endif()
else()
    message(STATUS "OptiX RT-core probe DISABLED (optix.h / cuda.h not found; "
            "set -DCAJETA_OPTIX_ROOT to enable). Software ray-query floor unaffected.")
endif()

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
    # ...but DON'T export the static LLVM symbols: HIP's libamd_comgr bundles its
    # own LLVM, and exporting ours interposes onto comgr's → SIGSEGV the moment
    # HIP initializes in-process. Hide just the LLVM archives (the __cajeta_*
    # runtime symbols the JIT dlsym's stay exported). Unblocks in-process AMD
    # device tests. See src/CMakeLists.txt (CAJETA_LLVM_EXCLUDE_LIBS).
    # --exclude-libs is GNU-ld/LLD-only; Apple's ld64 rejects it (and macOS has no
    # HIP to clash with), so skip it there.
    if(CAJETA_LLVM_EXCLUDE_LIBS AND NOT APPLE)
        target_link_options(${BINARY} PRIVATE
                "-Wl,--exclude-libs,${CAJETA_LLVM_EXCLUDE_LIBS}")
    endif()
    # --exclude-libs only reaches ARCHIVES; LLVM's headers also emit vague-linkage
    # template instantiations into cajeta's own objects, which the script localizes.
    if(CAJETA_LLVM_VERSION_SCRIPT AND NOT APPLE)
        target_link_options(${BINARY} PRIVATE
                "-Wl,--version-script=${CAJETA_LLVM_VERSION_SCRIPT}")
        set_property(TARGET ${BINARY} APPEND PROPERTY
                LINK_DEPENDS "${CAJETA_LLVM_VERSION_SCRIPT}")
    endif()
endif()

# The build-tool HTTP test server (test/buildtool/TestHttpServer.h) speaks
# Winsock2 on Windows; link the sockets library.
if(WIN32)
    target_link_libraries(${BINARY} PRIVATE ws2_32)
endif()

# Windows: keep the libLLVM DLL beside the test exe IN LOCKSTEP with the linked
# LLVM. cajeta_test links the monolithic shared libLLVM-<ver>.dll, and Windows
# resolves DLLs from the EXECUTABLE'S OWN DIRECTORY before PATH — so a stale copy
# next to the exe silently shadows the fork DLL the runner (cajeta_tests.ps1) puts
# on PATH. That bit us hard: a hand-dropped libLLVM-23git.dll predating the
# SPV_KHR_quad_control commit made EVERY Vulkan SPIR-V emit fail with "Unknown
# SPIR-V extension" → segfault, while `llc` (next to the fresh build/bin DLL)
# worked. A POST_BUILD copy_if_different from the LLVM toolchain bin (the dir
# find_package(LLVM) resolved, re-globbed each configure) keeps the exe-dir copy
# current after every (re)link. No-op for a static LLVM toolchain (no DLL found).
if(WIN32 AND LLVM_DIR)
    get_filename_component(CAJETA_TEST_LLVM_BIN "${LLVM_DIR}/../../../bin" ABSOLUTE)
    file(GLOB CAJETA_TEST_LLVM_DLLS "${CAJETA_TEST_LLVM_BIN}/libLLVM-*.dll")
    if(CAJETA_TEST_LLVM_DLLS)
        add_custom_command(TARGET ${BINARY} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${CAJETA_TEST_LLVM_DLLS} "$<TARGET_FILE_DIR:${BINARY}>"
                COMMENT "Refreshing libLLVM DLL beside ${BINARY} (prevents stale-DLL shadowing)")
    endif()
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
#
# DISCOVERY_MODE: on Windows the test exe links the monolithic libLLVM DLL
# plus the mingw-runtime / antlr4 / zstd / glog / openssl / curl / libxml2
# DLLs, which are only assembled onto PATH by the test runner scripts
# (cajeta_tests.ps1 / release_tests.cmd) — NOT during the build. The default
# POST_BUILD discovery launches the exe at link time to enumerate tests; in
# that bare build environment Windows can't load the dependent DLLs, so the
# process fails to start (ERROR_BAD_EXE_FORMAT, surfaced by CMake as
# "inappropriate file type or format" with empty output) and breaks the build.
# Defer discovery to test time on Windows so enumeration runs under the
# runner's curated PATH. Linux/macOS resolve their .so deps at build time, so
# keep the default (build-time) discovery there.
if(WIN32)
    gtest_discover_tests(${BINARY}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES TIMEOUT 120)
else()
    gtest_discover_tests(${BINARY}
            PROPERTIES TIMEOUT 120)
endif()

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
