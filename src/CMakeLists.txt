CMAKE_MINIMUM_REQUIRED(VERSION 3.25)

LIST(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 17)
cmake_policy(SET CMP0135 NEW)
cmake_policy(SET CMP0075 NEW)

# Locate ANTLR4 jar (used by Java tool to generate C++ lexer/parser).
# Allow override via env var ANTLR_EXECUTABLE; otherwise use bundled jar.
if(NOT DEFINED ENV{ANTLR_EXECUTABLE})
    find_file(ANTLR4_JAR
            NAMES antlr-4.10-complete.jar antlr-4.10.1-complete.jar
            PATHS ${PROJECT_SOURCE_DIR}/tools/antlr /usr/local/share/java /usr/share/java
            NO_DEFAULT_PATH)
    if(ANTLR4_JAR)
        set(ENV{ANTLR_EXECUTABLE} ${ANTLR4_JAR})
    endif()
endif()
set(Java_JAVA_EXECUTABLE "java")
set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})
add_definitions(-DANTLR4CPP_STATIC)

find_package(glog 0.6.0 REQUIRED)
find_package(GTest REQUIRED)
# zlib backs the embedded-IDE-plugin extraction (`cajeta ide install`): the
# plugin distribution zip's DEFLATE entries are raw-inflated via zlib. Linked
# unconditionally (the lld branch below also uses it, but that path is optional).
find_package(ZLIB REQUIRED)
# libcrypto from OpenSSL backs `cajeta archive sign` / `verify-sig`
# (ed25519 detached signatures + PEM key parsing). See
# docs/ArchiveManagement.md §8.
find_package(OpenSSL REQUIRED)

# libcurl powers the HTTP repository driver (Phase 6b: bearer + mTLS
# auth, GETs against the v1 registry REST API). Linked PUBLIC so the
# header surface is reachable from test code that exercises the
# in-process HTTP fixture server.
find_package(CURL REQUIRED)
# zlib is used unconditionally by the OCI image writer (Phase 9, gzip
# layer encoding). It's also pulled in by lld below — that block stays
# but is now redundant on the linker side. Doubling the find_package
# is a no-op (CMake caches the target).
find_package(ZLIB REQUIRED)

# LLVM (since 18) expects an IMPORTED target zstd::libzstd_shared.
# Prefer the proper find_package(zstd CONFIG) path — MSYS2 (mingw-w64),
# vcpkg, and most modern distros ship zstdConfig.cmake. Falls back to
# a manual find_library + add_library(IMPORTED) only when no cmake
# config is available (e.g. older Ubuntu where libzstd-dev didn't ship
# the config). The fallback covers multi-arch Linux paths
# (x86_64 + aarch64), macOS Homebrew prefixes, and the default cmake
# search paths.
#
# IMPORTANT: don't define our fallback IMPORTED target if find_package
# succeeded — zstdConfig.cmake defines a whole family
# (libzstd_shared, libzstd_static, libzstd) and errors out if one of
# them was pre-defined by us.
find_package(zstd CONFIG QUIET)
if(NOT TARGET zstd::libzstd_shared)
    # MSYS2 ships no zstdConfig.cmake, so find_package(CONFIG) above misses and
    # we land here. Add the mingw-w64 lib dir as a HINT (Windows, dir-exists
    # guarded) so a bare `cmake` from a shell without mingw64 on PATH still
    # finds libzstd.dll.a — matching the clang/xxd HINTS below and the
    # top-level CMAKE_PREFIX_PATH seed.
    set(_cajeta_zstd_win_hint "")
    if(WIN32 AND EXISTS "C:/msys64/mingw64/lib")
        set(_cajeta_zstd_win_hint "C:/msys64/mingw64/lib")
    endif()
    find_library(ZSTD_LIBRARY
            NAMES zstd libzstd.so libzstd.so.1 libzstd.dylib libzstd.1.dylib
            HINTS
                ${_cajeta_zstd_win_hint}
                /opt/homebrew/opt/zstd/lib
                /usr/local/opt/zstd/lib
                $ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib
                C:/vcpkg/installed/x64-windows/lib
            PATHS
                /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
                /usr/lib
                /usr/local/lib)
    if(ZSTD_LIBRARY)
        add_library(zstd::libzstd_shared SHARED IMPORTED)
        set_target_properties(zstd::libzstd_shared PROPERTIES
                IMPORTED_LOCATION ${ZSTD_LIBRARY})
        message(STATUS "Created fallback zstd::libzstd_shared -> ${ZSTD_LIBRARY}")
    else()
        message(FATAL_ERROR "Could not locate libzstd. Install libzstd-dev (Linux), brew install zstd (macOS), or vcpkg install zstd (Windows).")
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)
# LLVM_DEFINITIONS is a single space-separated string (e.g.
# "-D_GLIBCXX_USE_CXX11_ABI=1 -D__STDC_CONSTANT_MACROS ..."). Passing it to
# add_definitions() verbatim collapses the whole string into ONE malformed
# definition, so the first macro's value swallows the remaining -D tokens
# (_GLIBCXX_USE_CXX11_ABI becomes "1 -D_FILE_OFFSET_BITS=64 ..."). Most
# tokens are never used in an #if so this stayed silent on some toolchains,
# but mingw LLVM's string defines _GLIBCXX_USE_CXX11_ABI=1, which libstdc++
# evaluates in #if and rejects ("token '=' is not valid"). Split into a
# proper list first.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(${LLVM_INCLUDE_DIRS})

# Detect whether this LLVM carries the cajeta-llvm fork's SPV_KHR_ray_query
# intrinsics (llvm.spv.ray.query.*). The ray-query lowering references the
# intrinsic enums directly, which is a hard compile error against stock upstream
# LLVM (it lacks them). When absent, cajeta still builds and a RayQuery in a
# kernel becomes a clean XPU-N02 diagnostic instead (SpirvKernelLowering.cpp).
# This unblocks release legs that haven't yet got a fork toolchain artifact.
# See plans/c0 / cajeta-llvm CAJETA-FORK.md.
include(CheckCXXSourceCompiles)
# Re-detect every configure: the LLVM toolchain can change between configures
# (fork <-> stock) and the result must track it. Compile-ONLY (STATIC_LIBRARY
# target type) so a spurious LINK failure doesn't mask a present enum — a plain
# check_cxx_source_compiles links a full executable and can fail at ld for
# reasons unrelated to whether the enum exists.
unset(CAJETA_HAS_SPV_RAY_QUERY CACHE)
set(_CAJETA_RQ_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
set(_CAJETA_RQ_FLAGS "${CMAKE_REQUIRED_FLAGS}")
set(_CAJETA_RQ_TTYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_REQUIRED_INCLUDES ${LLVM_INCLUDE_DIRS})
set(CMAKE_REQUIRED_FLAGS "-std=c++17 ${LLVM_DEFINITIONS}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_source_compiles("
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsSPIRV.h>
int probe() { return (int) llvm::Intrinsic::spv_ray_query_initialize; }
" CAJETA_HAS_SPV_RAY_QUERY)
set(CMAKE_REQUIRED_INCLUDES "${_CAJETA_RQ_INCLUDES}")
set(CMAKE_REQUIRED_FLAGS "${_CAJETA_RQ_FLAGS}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_CAJETA_RQ_TTYPE}")
if(CAJETA_HAS_SPV_RAY_QUERY)
    message(STATUS "cajeta: LLVM has SPV_KHR_ray_query intrinsics (fork toolchain) -- ray query ENABLED")
else()
    message(STATUS "cajeta: LLVM lacks SPV_KHR_ray_query intrinsics (stock LLVM) -- ray query DISABLED (clean XPU-N02 diagnostic)")
endif()

# Same probe for the cooperative-matrix intrinsics (CM2). They live in the same
# cajeta-llvm fork, but probe independently so the guard is honest about what it
# gates (and a future split fork still works).
unset(CAJETA_HAS_SPV_COOP_MATRIX CACHE)
set(_CAJETA_CM_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
set(_CAJETA_CM_FLAGS "${CMAKE_REQUIRED_FLAGS}")
set(_CAJETA_CM_TTYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_REQUIRED_INCLUDES ${LLVM_INCLUDE_DIRS})
set(CMAKE_REQUIRED_FLAGS "-std=c++17 ${LLVM_DEFINITIONS}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_source_compiles("
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsSPIRV.h>
int probe() { return (int) llvm::Intrinsic::spv_cooperative_matrix_load; }
" CAJETA_HAS_SPV_COOP_MATRIX)
set(CMAKE_REQUIRED_INCLUDES "${_CAJETA_CM_INCLUDES}")
set(CMAKE_REQUIRED_FLAGS "${_CAJETA_CM_FLAGS}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_CAJETA_CM_TTYPE}")
if(CAJETA_HAS_SPV_COOP_MATRIX)
    message(STATUS "cajeta: LLVM has SPV_KHR_cooperative_matrix intrinsics (fork toolchain) -- cooperative matrix ENABLED")
else()
    message(STATUS "cajeta: LLVM lacks SPV_KHR_cooperative_matrix intrinsics (stock LLVM) -- cooperative matrix DISABLED (clean XPU-N03 diagnostic)")
endif()

find_package(ANTLR REQUIRED)

# System antlr4 runtime headers + library. Portability: search across
# multi-arch Linux paths, Homebrew prefixes (Intel + ARM), and vcpkg's
# installed root on Windows. NAMES covers Linux .so, macOS .dylib, and
# the MSVC `antlr4-runtime.lib` form via cmake's default suffix
# mangling on the bare `antlr4-runtime` name.
find_path(ANTLR4_INCLUDE_DIR
        NAMES antlr4-runtime.h
        PATH_SUFFIXES antlr4-runtime
        HINTS
            /opt/homebrew/opt/antlr4-cpp-runtime/include
            /usr/local/opt/antlr4-cpp-runtime/include
            $ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/include
            C:/vcpkg/installed/x64-windows/include
        PATHS /usr/include /usr/local/include)
find_library(ANTLR4_RUNTIME_LIB
        NAMES antlr4-runtime
        HINTS
            /opt/homebrew/opt/antlr4-cpp-runtime/lib
            /usr/local/opt/antlr4-cpp-runtime/lib
            $ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib
            C:/vcpkg/installed/x64-windows/lib
        PATHS
            /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
            /usr/lib
            /usr/local/lib)
if(NOT ANTLR4_INCLUDE_DIR OR NOT ANTLR4_RUNTIME_LIB)
    message(FATAL_ERROR "Could not locate antlr4-runtime headers/library. Install libantlr4-runtime-dev (Linux), brew install antlr4-cpp-runtime (macOS), or vcpkg install antlr4 (Windows).")
endif()
include_directories(${ANTLR4_INCLUDE_DIR})

message(STATUS "ANTLR_EXECUTABLE: ${ANTLR_EXECUTABLE}")
message(STATUS "ANTLR4_INCLUDE_DIR: ${ANTLR4_INCLUDE_DIR}")
message(STATUS "ANTLR4_RUNTIME_LIB: ${ANTLR4_RUNTIME_LIB}")

antlr_target(CajetaGrammarLexer ${CMAKE_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
        PACKAGE cajeta OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/lex")
antlr_target(CajetaGrammarParser ${CMAKE_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER VISITOR
        PACKAGE cajeta
        DEPENDS_ANTLR CajetaGrammarLexer
        COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
        OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/parser")

## include generated files in project environment
include_directories(${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})
include_directories(${ANTLR_CajetaGrammarParser_OUTPUT_DIR})

# Named global target standing in for "the ANTLR lexer/parser has been
# generated". cajeta_lib lists the generated .cpp's as sources so it already
# orders after generation, but OTHER targets that merely #include the generated
# headers without compiling the sources (notably cajetadoc_core, which pulls in
# CajetaParser.h) have no such ordering. Without an explicit dependency a
# parallel build can compile those TUs before ANTLR writes the header, failing
# intermittently with "CajetaParser.h: No such file or directory". The
# dependency is wired up at the top-level CMakeLists (cajetadoc is configured
# before src/, so the edge is added once both targets exist).
add_custom_target(cajeta_antlr_generated
        DEPENDS
            ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS}
            ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS})

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using LLVM includes at: ${LLVM_INCLUDE_DIRS}")
message(STATUS "Using project includes at: ${CMAKE_INCLUDE_SOURCE_DIR}/include")
# (The macOS brew-lib link-search-path fix lives in the TOP-level
# CMakeLists.txt so it propagates to test/ targets as well as src/.)

# Resolve LLVM libraries. The compiler initializes all targets at startup,
# so we need every backend linked in.
#
# On Windows/MinGW where LLVM is packaged as a single shared library
# (LLVM_LINK_LLVM_DYLIB ON, the MSYS2 default), link that one dylib instead of
# the static component archives. This is REQUIRED on MSYS2: the lld static libs
# there were built against libLLVM.dll, so their references into llvm:: (the
# demangler, llvm::lto::DTLTO, ...) are PE __declspec(dllimport) and resolve
# ONLY against the LLVM import library — the static component .a's cannot
# satisfy `__imp_`-prefixed refs. The dylib also bundles every component (LTO,
# Demangle, ...), so no further component juggling is needed.
#
# Scoped to WIN32: the dllimport problem is PE-specific. Linux/macOS keep the
# static-component linking they already ship green with, even if their LLVM
# package happens to set LLVM_LINK_LLVM_DYLIB.
if(WIN32 AND LLVM_LINK_LLVM_DYLIB)
    set(CAJETA_USE_LLVM_DYLIB ON)
else()
    set(CAJETA_USE_LLVM_DYLIB OFF)
endif()

if(CAJETA_USE_LLVM_DYLIB)
    set(LLVM_LIBS LLVM)
    message(STATUS "Linking the monolithic LLVM dylib (Windows + LLVM_LINK_LLVM_DYLIB)")
else()
    llvm_map_components_to_libnames(LLVM_LIBS
            support core irreader option
            target mc passes analysis bitwriter bitreader
            orcjit orcdebugging jitlink executionengine runtimedyld linker
            AllTargetsAsmParsers
            AllTargetsCodeGens
            AllTargetsDescs
            AllTargetsInfos)
endif()

# Optional: detect lld libraries (lld-${LLVM_VERSION_MAJOR}-dev on Ubuntu when
# the distro ships one — note Ubuntu 26.04+ dropped lld-N-dev for all N). When
# present we link the ELF driver in-process to support --emit=exe. When absent
# the compiler still builds; --emit=exe just prints an "install lld-N-dev"
# hint at runtime. LLVM_VERSION_MAJOR comes from find_package(LLVM CONFIG)
# above, so the lld discovery automatically tracks whichever LLVM was found
# (no separate hardcoded major version).
find_path(CAJETA_LLD_INCLUDE_DIR
        NAMES lld/Common/Driver.h
        PATHS /usr/include /usr/local/include
              /usr/lib/llvm-${LLVM_VERSION_MAJOR}/include
              /usr/include/llvm-${LLVM_VERSION_MAJOR})
find_library(CAJETA_LLD_COMMON_LIB NAMES lldCommon)
find_library(CAJETA_LLD_ELF_LIB NAMES lldELF)
if(CAJETA_LLD_INCLUDE_DIR AND CAJETA_LLD_COMMON_LIB AND CAJETA_LLD_ELF_LIB)
    # lld's ELF OutputSection compression references zlib (adler32, deflate*)
    # and zstd. zstd is already linked below; zlib is otherwise unused by
    # cajeta, so pull it in here, next to lld, the only consumer. Placed after
    # the lld archives so GNU ld's single left-to-right pass resolves lld's
    # zlib refs. On Linux-without-lld this stays unlinked (this branch is off).
    find_package(ZLIB REQUIRED)
    set(CAJETA_LLD_LIBS ${CAJETA_LLD_ELF_LIB} ${CAJETA_LLD_COMMON_LIB} ZLIB::ZLIB)
    set(CAJETA_HAS_LLD ON)
    message(STATUS "lld libraries found; --emit=exe will use lld in-process.")
    # lld's ThinLTO BitcodeCompiler references llvm::lto::* — pull libLLVMLTO
    # (and its transitive deps, which on some LLVM builds includes Polly) ONLY
    # when lld is actually linked AND we're using the static component libs.
    # Gating on lld keeps the Linux CI hard-gate build (no lld there) from
    # requiring libPolly.a, which apt's llvm-N-dev doesn't ship. Prepended so
    # it precedes the JIT LLVM libs, satisfying lld's references before GNU
    # ld's single left-to-right pass moves past. When we link the monolithic
    # dylib (CAJETA_USE_LLVM_DYLIB) LTO is already inside libLLVM, so skip this.
    if(NOT CAJETA_USE_LLVM_DYLIB)
        llvm_map_components_to_libnames(CAJETA_LLD_LLVM_DEPS lto)
        list(PREPEND LLVM_LIBS ${CAJETA_LLD_LLVM_DEPS})
    endif()
else()
    set(CAJETA_LLD_LIBS "")
    set(CAJETA_HAS_LLD OFF)
    message(STATUS "lld libraries NOT found; --emit=exe will print a hint. Install "
            "lld-${LLVM_VERSION_MAJOR}-dev (Debian/Ubuntu) to enable in-process linking.")
endif()

set(BINARY ${CMAKE_PROJECT_NAME})
# CONFIGURE_DEPENDS so ninja re-evaluates the glob (and reconfigures) when
# source files are added/removed. Without it, a build dir configured before a
# new .cpp existed silently omits that translation unit — its symbols then
# fail to link (the XPU lowering/vulkan files hit exactly this). The stdlib
# globs below already use CONFIGURE_DEPENDS; this keeps the C++ source glob
# consistent.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS LIST_DIRECTORIES false *.h *.cpp)
list(FILTER SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
# DocCommand.cpp (the `cajeta doc` forwarder) is compiled into the cajeta
# executable, NOT cajeta_lib, so wiring it to the cajetadoc engine doesn't add
# include/define usage-requirements to cajeta_lib and force a full recompile of
# the compiler. See the executable definition below.
list(FILTER SOURCES EXCLUDE REGEX ".*/cli/DocCommand\\.cpp$")

set(GENERATED_SOURCES
        ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS}
        ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS})

# --- Embedded Cajeta runtime --------------------------------------------------
# Compile the C runtime (runtime/native/cajeta_runtime.c) to LLVM bitcode at
# compiler-build time, then embed the bitcode bytes into the compiler binary
# via xxd -i. At codegen time, CajetaModule::linkRuntime() parses these bytes
# into an llvm::Module and Linker::linkModules-merges them into the user output.
# Distribution-portable: no per-target prebuilt .a required.
# Prefer the versioned clang that matches whichever LLVM was found above,
# falling back to plain `clang` on PATH. Keeping the versioned-first order
# matters on systems with multiple LLVMs installed side-by-side (e.g. apt
# llvm-21-dev coexisting with an older llvm-18 from the same archive) —
# without it, plain `clang` may resolve to a different major version than
# the LLVMConfig.cmake we're already linking against, and the bitcode it
# emits won't match what the host LLVM expects.
# Windows/MSYS2: clang + xxd live under the mingw-w64 (and base) prefixes,
# which aren't on a default shell's PATH. find_program searches PATH + these
# HINTS/PATHS, so seed the MSYS2 bin dirs (guarded to existing dirs) — same
# rationale as the CMAKE_PREFIX_PATH seed in the top-level CMakeLists. No effect
# on Linux/macOS, where /usr/bin etc. already cover it.
set(CAJETA_MSYS_BIN_HINTS "")
if(WIN32)
    foreach(_d "C:/msys64/mingw64/bin" "C:/msys64/usr/bin")
        if(EXISTS "${_d}")
            list(APPEND CAJETA_MSYS_BIN_HINTS "${_d}")
        endif()
    endforeach()
endif()
# The runtime bitcode (cajeta_runtime.bc + the fp128 builtins) MUST be produced
# by the SAME LLVM the cajeta compiler links and JITs against. Bitcode emitted by
# a different LLVM major silently mismatches the JIT that loads it — e.g. ROCm's
# AMD clang (LLVM 22) sitting on PATH ahead of the fork (LLVM 23). So pin the
# search to LLVM_TOOLS_BINARY_DIR (the fork's own bin dir, exported by the same
# LLVMConfig found above) FIRST, before any PATH clang, and verify the major
# below. Without the leading hint, find_program falls back to a bare `clang` on
# PATH and caches a foreign-LLVM toolchain.
find_program(CAJETA_CLANG NAMES "clang-${LLVM_VERSION_MAJOR}" clang
        HINTS ${LLVM_TOOLS_BINARY_DIR} ${CAJETA_MSYS_BIN_HINTS}
        PATHS /usr/lib/llvm-${LLVM_VERSION_MAJOR}/bin /usr/bin /usr/local/bin)
find_program(CAJETA_XXD NAMES xxd
        HINTS ${CAJETA_MSYS_BIN_HINTS}
        PATHS /usr/bin /usr/local/bin)
# llvm-link merges the fp128 builtin bitcode into the runtime bitcode (below).
# Same pin: prefer the fork's bin dir (and clang's own dir) over PATH.
get_filename_component(CAJETA_CLANG_BIN_DIR "${CAJETA_CLANG}" DIRECTORY)
find_program(CAJETA_LLVM_LINK NAMES "llvm-link-${LLVM_VERSION_MAJOR}" llvm-link
        HINTS ${LLVM_TOOLS_BINARY_DIR} ${CAJETA_CLANG_BIN_DIR} ${CAJETA_MSYS_BIN_HINTS}
        PATHS /usr/lib/llvm-${LLVM_VERSION_MAJOR}/bin /usr/bin /usr/local/bin)
if(NOT CAJETA_CLANG)
    message(FATAL_ERROR "clang not found; needed to compile cajeta runtime to bitcode.")
endif()
if(NOT CAJETA_XXD)
    message(FATAL_ERROR "xxd not found; install vim-common (Debian/Ubuntu) or equivalent.")
endif()
if(NOT CAJETA_LLVM_LINK)
    message(FATAL_ERROR "llvm-link not found; needed to merge the fp128 builtin bitcode into the runtime bitcode.")
endif()

# Hard-fail on a runtime-bitcode toolchain that is a different LLVM major than
# the compiler's LLVM. A loud configure error is vastly preferable to silently
# emitting the runtime bitcode with a foreign LLVM and crashing at JIT link time.
execute_process(COMMAND "${CAJETA_CLANG}" --version
        OUTPUT_VARIABLE CAJETA_CLANG_VERSION_OUT ERROR_QUIET)
if(CAJETA_CLANG_VERSION_OUT MATCHES "clang version ([0-9]+)")
    set(CAJETA_CLANG_MAJOR "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR
        "Could not determine the LLVM major of CAJETA_CLANG=${CAJETA_CLANG}; "
        "refusing to build the runtime bitcode with an unverifiable toolchain.")
endif()
if(NOT CAJETA_CLANG_MAJOR STREQUAL "${LLVM_VERSION_MAJOR}")
    message(FATAL_ERROR
        "Runtime-bitcode toolchain mismatch: CAJETA_CLANG=${CAJETA_CLANG} is LLVM "
        "${CAJETA_CLANG_MAJOR}, but the cajeta compiler links LLVM ${LLVM_VERSION_MAJOR} "
        "(LLVM_DIR=${LLVM_DIR}). The runtime bitcode would be compiled by a different "
        "LLVM than the JIT loads it into. Put clang-${LLVM_VERSION_MAJOR} from the same "
        "build ahead of any other clang (e.g. ROCm's) on PATH, or configure with "
        "-DCAJETA_CLANG=<llvm-tools-bin>/clang-${LLVM_VERSION_MAJOR}.")
endif()
message(STATUS "Runtime-bitcode toolchain: ${CAJETA_CLANG} (LLVM ${CAJETA_CLANG_MAJOR}, matches compiler)")

set(CAJETA_RT_SRC ${PROJECT_SOURCE_DIR}/runtime/native/cajeta_runtime.c)
set(CAJETA_RT_BC  ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime.bc)
set(CAJETA_RT_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime_embedded.cpp)
# Target-neutral fp128 soft-float conversion builtins (see the .ll header). The
# core runtime compiles to cajeta_runtime_core.bc; we assemble the builtins and
# llvm-link both into the final cajeta_runtime.bc that gets embedded — so every
# JIT module resolves __trunctfdf2 / __fixtfdi / ... internally (Apple arm64 has
# no fp128/__float128 and no compiler-rt tf family to fall back on).
set(CAJETA_FP128_LL  ${PROJECT_SOURCE_DIR}/runtime/native/cajeta_fp128_builtins.ll)
set(CAJETA_RT_CORE_BC ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime_core.bc)
set(CAJETA_FP128_BC  ${CMAKE_CURRENT_BINARY_DIR}/cajeta_fp128_builtins.bc)

# xxhash header location for the runtime compile. Debian/Ubuntu install
# at /usr/include (default path, no flag needed); macOS Homebrew puts it
# at /opt/homebrew/opt/xxhash/include (Apple Silicon) or
# /usr/local/opt/xxhash/include (Intel); Windows vcpkg at
# C:/vcpkg/installed/x64-windows/include. Look in all three.
find_path(CAJETA_XXHASH_INCLUDE_DIR
        NAMES xxhash.h
        HINTS
            /opt/homebrew/opt/xxhash/include
            /usr/local/opt/xxhash/include
            $ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/include
            C:/vcpkg/installed/x64-windows/include
        PATHS /usr/include /usr/local/include)
if(NOT CAJETA_XXHASH_INCLUDE_DIR)
    message(FATAL_ERROR "Could not locate xxhash.h. Install libxxhash-dev (Linux), brew install xxhash (macOS), or vcpkg install xxhash (Windows).")
endif()
message(STATUS "CAJETA_XXHASH_INCLUDE_DIR: ${CAJETA_XXHASH_INCLUDE_DIR}")

# Runtime compile flags. Pattern: avoid clang/LLVM lowerings that
# reference compiler-rt helpers, because the LLJIT can't materialize
# those symbols at runtime — every helper miss surfaces as
# "Symbols not found: [ ___FOO ]" when the test harness loads the
# embedded runtime bitcode into a JIT module.
#
#   - aarch64-linux: -mno-outline-atomics forces inline LSE `ldadd`
#     etc. (armv8.1+) instead of __aarch64_outline_atomics_* helpers.
#     Modern ARM CPUs (post-2017) all have LSE; fine for our targets.
#   - Apple targets (macOS arm64 / Intel): -fno-emulated-tls makes
#     clang emit real dyld TLS calls (_tlv_bootstrap) for the
#     runtime's __thread variables instead of the __emutls_get_address
#     helper. LLJIT can resolve dyld's TLS via DynamicLibrarySearchGenerator;
#     the emutls helper isn't on its search path.
set(CAJETA_RT_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CAJETA_RT_FLAGS "-mno-outline-atomics")
endif()
if(WIN32)
    # The runtime TU includes <windows.h>, and clang (unlike the gcc that
    # builds the rest of cajeta on mingw) emits a flood of warnings from the
    # MSYS2 Windows SDK headers themselves -- ~28 -Wpragma-pack from winnt.h/
    # wingdi.h/winuser.h push/pop'ing #pragma pack across includes, and
    # -Wmissing-declarations from mstcpip.h's INET_PORT_RESERVATION typedefs.
    # These are system-header noise, not cajeta defects (the other OSes don't
    # pull in these headers, so they never saw them). Silence them so a genuine
    # runtime-compile warning isn't buried. Windows-only, output-only: this does
    # not change the emitted bitcode.
    list(APPEND CAJETA_RT_FLAGS "-Wno-pragma-pack" "-Wno-missing-declarations")
endif()
if(APPLE)
    list(APPEND CAJETA_RT_FLAGS "-fno-emulated-tls")
    # The fork's clang-23 is a vanilla LLVM build with no embedded macOS SDK,
    # so <math.h> and the other system headers aren't on its default search
    # path (Apple's own clang resolves these via xcrun). Point it at the
    # active SDK explicitly, or the runtime-bitcode compile fails with
    # "'math.h' file not found".
    execute_process(
            COMMAND xcrun --show-sdk-path
            OUTPUT_VARIABLE CAJETA_MACOS_SDK
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    if(CAJETA_MACOS_SDK)
        list(APPEND CAJETA_RT_FLAGS "-isysroot" "${CAJETA_MACOS_SDK}")
        message(STATUS "CAJETA_RT macOS SDK: ${CAJETA_MACOS_SDK}")
    endif()
endif()

# cajeta_runtime.c is a single TU that `#include`s the per-subsystem native
# sources (cajeta_net_*.c, cajeta_hash_*.c, …). A bare `DEPENDS` on the top
# file would NOT rebuild the bitcode when one of those included .c files
# changes — clang emits no dependency info without `-MD`. Emit a depfile and
# wire it through `DEPFILE` so every transitive include is tracked (Ninja);
# otherwise edits to an included runtime source silently ship a stale bitcode
# (a real bug: a new intrinsic added to e.g. cajeta_net_socket.c would be
# missing from the embedded runtime, so getRuntimeFunction can't find it).
# -O3 (not -O2): the embedded bitcode is OPTIMIZED here, once, and that opt
# level is baked in — the AOT pipeline re-codegens it for the native target but
# does NOT redo target-aware loop transforms (unroll factor, vectorization width)
# on already-optimized IR, so -O2's weaker transforms persist into every @Native
# bulk path. Bumping to -O3 measured: blake3 1.43x, md5 1.21x, sha256 1.12x over
# a 1 MiB buffer (and lands blake3 at the standalone -O3 ceiling). Stays GENERIC
# on purpose — do NOT add -march=native: this bitcode ships in the DISTRIBUTED
# compiler and is JIT-loaded on arbitrary hosts, and the AVX-512 hot paths
# (blake3/sha256/xxh3) already gate their own intrinsics behind
# __attribute__((target("avx512f"))) + a cpuid probe, so baking a fixed CPU in
# only degrades the AOT re-codegen (measured: -march=native made blake3 SLOWER,
# 224us vs 189us generic).
add_custom_command(
        OUTPUT ${CAJETA_RT_BC}
        # 1) Core runtime C -> bitcode (was the whole step; now an intermediate).
        COMMAND ${CAJETA_CLANG} -c -emit-llvm -O3 -fno-builtin
                -ffunction-sections -fdata-sections
                -MD -MF ${CAJETA_RT_CORE_BC}.d
                -I${CAJETA_XXHASH_INCLUDE_DIR}
                ${CAJETA_RT_FLAGS}
                -o ${CAJETA_RT_CORE_BC} ${CAJETA_RT_SRC}
        # 2) Assemble the vendored target-neutral fp128 builtins (.ll -> .bc).
        COMMAND ${CAJETA_CLANG} -c -emit-llvm -x ir
                -o ${CAJETA_FP128_BC} ${CAJETA_FP128_LL}
        # 3) Merge into the final embedded bitcode. The builtins are
        #    linkonce_odr, so on hosts whose libgcc also defines them there is
        #    no conflict.
        COMMAND ${CAJETA_LLVM_LINK}
                ${CAJETA_RT_CORE_BC} ${CAJETA_FP128_BC}
                -o ${CAJETA_RT_BC}
        DEPENDS ${CAJETA_RT_SRC} ${CAJETA_FP128_LL}
        DEPFILE ${CAJETA_RT_CORE_BC}.d
        COMMENT "Compiling Cajeta runtime + fp128 builtins to LLVM bitcode")

# xxd -i derives the C variable name from the input filename, so we run it in
# the binary directory with a relative input path to get clean symbol names.
add_custom_command(
        OUTPUT ${CAJETA_RT_EMBED}
        COMMAND ${CAJETA_XXD} -i cajeta_runtime.bc ${CAJETA_RT_EMBED}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CAJETA_RT_BC}
        COMMENT "Embedding Cajeta runtime bitcode")

set_source_files_properties(${CAJETA_RT_EMBED} PROPERTIES GENERATED TRUE)
list(APPEND GENERATED_SOURCES ${CAJETA_RT_EMBED})

# Also compile the runtime as a native object so tests can observe runtime
# state (e.g. drop counters) directly from C++. The JIT'd code still uses the
# embedded bitcode version; the native one only matters for the test binary.
set_source_files_properties(${CAJETA_RT_SRC} PROPERTIES LANGUAGE C)
list(APPEND GENERATED_SOURCES ${CAJETA_RT_SRC})

# cajeta.io.net TLS engine (NET-5.1). Compiled as a STANDALONE native C object —
# NOT #included into cajeta_runtime.c — so OpenSSL's headers stay out of the
# embedded JIT bitcode (which is loaded into every JIT test). The `__cajeta_tls_*`
# symbols land in libcajeta_lib, so the test binary calls them directly and the
# JIT resolves them through its process-symbol generator at @Native call sites.
set(CAJETA_TLS_SRC ${PROJECT_SOURCE_DIR}/runtime/native/cajeta_tls.c)
set_source_files_properties(${CAJETA_TLS_SRC} PROPERTIES LANGUAGE C)
list(APPEND GENERATED_SOURCES ${CAJETA_TLS_SRC})

# ── AOT standalone-exe support (ALL platforms). Embed the COMPILED TLS native
# object into the compiler binary so `--emit=exe` can produce a self-contained
# executable. Background: a produced exe references `__cajeta_tls_*` from the
# stdlib TlsConnection @Native thunks, which are ALWAYS linked — cajeta roots
# every class's metadata via eager clinit global-constructors, so `--gc-sections`
# can't drop the unused TLS/net subsystem (true on ELF as well as PE-COFF — an
# orthogonal, deeper codegen change). Those natives live in this standalone
# object (kept out of the embedded JIT bitcode on purpose, see above), and the
# machine running `--emit=exe` has a C toolchain but not cajeta's runtime source
# — so the object bytes ride along in the compiler binary (mirrors the
# runtime-bitcode embed) and Compiler::linkExecutable materializes + links them
# alongside `-lssl -lcrypto`. Compiled with the build's own C compiler so the
# object ABI matches the C link driver used for the produced exe. Originally
# Windows-only; extended to Linux/macOS so `--emit=exe` links there too (the
# undefined `__cajeta_tls_*` symbols broke ANY exe, not just TLS-using programs).
set(CAJETA_TLS_OBJ   ${CMAKE_CURRENT_BINARY_DIR}/cajeta_tls.o)
set(CAJETA_TLS_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_tls_embedded.cpp)
set(CAJETA_TLS_CFLAGS -c -O2 -fno-builtin -ffunction-sections -fdata-sections)
if(NOT WIN32)
    # The cajeta-emitted objects are PIC (so they link into the default PIE
    # executable); compile the TLS native PIC too for a clean ELF/Mach-O link.
    list(APPEND CAJETA_TLS_CFLAGS -fPIC)
endif()
add_custom_command(
        OUTPUT ${CAJETA_TLS_OBJ}
        COMMAND ${CMAKE_C_COMPILER} ${CAJETA_TLS_CFLAGS}
                -I${OPENSSL_INCLUDE_DIR}
                -o ${CAJETA_TLS_OBJ} ${CAJETA_TLS_SRC}
        DEPENDS ${CAJETA_TLS_SRC}
        COMMENT "Compiling cajeta TLS native object for AOT exe embedding")
add_custom_command(
        OUTPUT ${CAJETA_TLS_EMBED}
        COMMAND ${CAJETA_XXD} -i cajeta_tls.o ${CAJETA_TLS_EMBED}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CAJETA_TLS_OBJ}
        COMMENT "Embedding cajeta TLS native object")
set_source_files_properties(${CAJETA_TLS_EMBED} PROPERTIES GENERATED TRUE)
list(APPEND GENERATED_SOURCES ${CAJETA_TLS_EMBED})

# Embed the cajeta stdlib `.cajeta` source files into the compiler binary.
# The Compiler iterates the resulting cajeta::stdlib::g_files manifest at
# parse time, treating each entry as a stdlib prelude source. Sources live
# under runtime/src/cajeta/ — see runtime/src/cajeta/error/ for the v1 set.
# Keeping them as real files (rather than inline C strings) makes the
# stdlib reviewable / editable like any other cajeta code.
set(CAJETA_STDLIB_ROOT ${PROJECT_SOURCE_DIR}/runtime/src)
# Recursive discovery rather than a hand-maintained allowlist: every
# `.cajeta` under runtime/src is embedded, so a new package (e.g. a new
# cajeta.search.* module) is picked up automatically with no edit here.
#
# Two knobs keep this safe:
#   * CAJETA_STDLIB_EXCLUDE_REGEX gates subtrees that aren't embeddable yet —
#     skeletal stubs and the socket-backed net files whose receiver-dispatch
#     lowering isn't implemented (eager-compiling them crashes the stdlib
#     build). These are isolated under their own subdirs (e.g.
#     cajeta/io/net/**/socket, cajeta/lang/string) so a path regex gates them
#     cleanly. They currently hold no sources; the regex is the forward guard.
#   * cajeta/lang is pinned FIRST so Object parses before the classes that
#     auto-extend it. Cross-file forward references (fields and `extends`) and
#     inherited-field layout otherwise resolve via the placeholder +
#     CajetaModule::buildPendingPrototypes fixed-point pass regardless of
#     order — so the remaining files just sort deterministically.
set(CAJETA_STDLIB_EXCLUDE_REGEX "/cajeta/(io/net/([^/]+/)*socket(/|$)|lang/string/)")
file(GLOB_RECURSE CAJETA_STDLIB_ALL CONFIGURE_DEPENDS ${CAJETA_STDLIB_ROOT}/*.cajeta)
set(CAJETA_STDLIB_LANG "")
set(CAJETA_STDLIB_REST "")
foreach(src_path ${CAJETA_STDLIB_ALL})
    if(src_path MATCHES "${CAJETA_STDLIB_EXCLUDE_REGEX}")
        continue()
    endif()
    if(src_path MATCHES "/cajeta/lang/")
        list(APPEND CAJETA_STDLIB_LANG ${src_path})
    else()
        list(APPEND CAJETA_STDLIB_REST ${src_path})
    endif()
endforeach()
list(SORT CAJETA_STDLIB_LANG)
list(SORT CAJETA_STDLIB_REST)
set(CAJETA_STDLIB_FILES ${CAJETA_STDLIB_LANG} ${CAJETA_STDLIB_REST})
list(LENGTH CAJETA_STDLIB_FILES CAJETA_STDLIB_COUNT)
set(CAJETA_STDLIB_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_stdlib_embedded.cpp)
set(CAJETA_STDLIB_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/cajeta_stdlib_manifest.txt)

# Build-time embed (via cmake/EmbedStdlib.cmake). Previously this
# generation happened at configure time; that meant editing a stdlib
# .cajeta file then running `ninja` left the embed stale until you
# re-ran `cmake .`. Now ninja re-runs the embedder whenever any
# listed source changes.
#
# The manifest file is the trick: passing the list directly through
# `-DCAJETA_STDLIB_FILES=a;b;c` collapses the semicolons to argv-
# splits at the sub-cmake invocation. Writing the list to a file
# at configure time and reading it back in the embed script side-
# steps that. CONFIGURE_DEPENDS on the globs above re-runs cmake
# (and thus regenerates the manifest) when files are added/removed,
# while file edits trip the custom-command's DEPENDS directly.
file(WRITE "${CAJETA_STDLIB_MANIFEST}" "")
foreach(src_path ${CAJETA_STDLIB_FILES})
    file(APPEND "${CAJETA_STDLIB_MANIFEST}" "${src_path}\n")
endforeach()

add_custom_command(
    OUTPUT ${CAJETA_STDLIB_EMBED}
    COMMAND ${CMAKE_COMMAND}
            -DMANIFEST=${CAJETA_STDLIB_MANIFEST}
            -DSTDLIB_ROOT=${CAJETA_STDLIB_ROOT}
            -DEMBED_OUT=${CAJETA_STDLIB_EMBED}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedStdlib.cmake
    DEPENDS ${CAJETA_STDLIB_FILES}
            ${CAJETA_STDLIB_MANIFEST}
            ${PROJECT_SOURCE_DIR}/cmake/EmbedStdlib.cmake
    COMMENT "Embedding ${CAJETA_STDLIB_COUNT} stdlib .cajeta sources into ${CAJETA_STDLIB_EMBED}"
    VERBATIM
)

set_source_files_properties(${CAJETA_STDLIB_EMBED} PROPERTIES GENERATED TRUE)
list(APPEND GENERATED_SOURCES ${CAJETA_STDLIB_EMBED})
# ------------------------------------------------------------------------------

# --- Embedded stdlib SKILL corpus (skill-discovery §2.4/§2.5) ----------------
# A tiny host codegen tool (links libzstd) packs every
# runtime/src/cajeta/<pkg>/skills/*.md into ONE zstd-compressed blob, embedded
# into the compiler so stdlib skills are ALWAYS available (no .cja / lockfile)
# while the binary carries them compressed. Decompressed on first access by
# src/cajeta/buildtool/skill/EmbeddedStdlibSkills.cpp.
add_executable(skillembed ${PROJECT_SOURCE_DIR}/tools/skillembed/SkillEmbed.cpp)
target_link_libraries(skillembed PRIVATE zstd::libzstd_shared)

file(GLOB CAJETA_STDLIB_SKILLS CONFIGURE_DEPENDS
     ${CAJETA_STDLIB_ROOT}/cajeta/*/skills/*.md)
list(SORT CAJETA_STDLIB_SKILLS)
list(LENGTH CAJETA_STDLIB_SKILLS CAJETA_STDLIB_SKILLS_COUNT)
set(CAJETA_STDLIB_SKILLS_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_stdlib_skills_embedded.cpp)
set(CAJETA_STDLIB_SKILLS_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/cajeta_stdlib_skills_manifest.txt)
file(WRITE "${CAJETA_STDLIB_SKILLS_MANIFEST}" "")
foreach(skill_path ${CAJETA_STDLIB_SKILLS})
    file(APPEND "${CAJETA_STDLIB_SKILLS_MANIFEST}" "${skill_path}\n")
endforeach()

add_custom_command(
    OUTPUT ${CAJETA_STDLIB_SKILLS_EMBED}
    COMMAND skillembed ${CAJETA_STDLIB_SKILLS_MANIFEST} ${CAJETA_STDLIB_ROOT}
            ${CAJETA_STDLIB_SKILLS_EMBED}
    DEPENDS skillembed ${CAJETA_STDLIB_SKILLS} ${CAJETA_STDLIB_SKILLS_MANIFEST}
    COMMENT "Embedding ${CAJETA_STDLIB_SKILLS_COUNT} stdlib skills (zstd) into ${CAJETA_STDLIB_SKILLS_EMBED}"
    VERBATIM
)
set_source_files_properties(${CAJETA_STDLIB_SKILLS_EMBED} PROPERTIES GENERATED TRUE)
list(APPEND GENERATED_SOURCES ${CAJETA_STDLIB_SKILLS_EMBED})
# ------------------------------------------------------------------------------

# --- Embedded IntelliJ IDEA plugin (plan §7) ---------------------------------
# Embed the plugin distribution zip into the compiler binary via xxd -i (same
# mechanism as the runtime bitcode), so `cajeta ide install` can extract it
# into the IDE plugins dir from any distribution form (D8). The Gradle build
# (release.yml §10) drops Cajeta-<ver>.zip under build/distributions/ before
# this configure runs. When no zip is present (a configure before the plugin
# build, e.g. a fresh dev tree), embed a zero-length stub so the build still
# links; `cajeta ide install` then reports no bundled plugin.
set(CAJETA_PLUGIN_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_plugin_embedded.cpp)
file(GLOB CAJETA_PLUGIN_ZIPS CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/ide-plugins/idea/build/distributions/*.zip)
if(CAJETA_PLUGIN_ZIPS)
    list(GET CAJETA_PLUGIN_ZIPS 0 CAJETA_PLUGIN_ZIP)
    # xxd derives the C symbol from the input filename, so copy to a stable
    # name (cajeta_plugin.zip) first → symbols cajeta_plugin_zip[/ _len].
    add_custom_command(
        OUTPUT ${CAJETA_PLUGIN_EMBED}
        COMMAND ${CMAKE_COMMAND} -E copy ${CAJETA_PLUGIN_ZIP}
                ${CMAKE_CURRENT_BINARY_DIR}/cajeta_plugin.zip
        COMMAND ${CAJETA_XXD} -i cajeta_plugin.zip ${CAJETA_PLUGIN_EMBED}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CAJETA_PLUGIN_ZIP}
        COMMENT "Embedding IDE plugin ${CAJETA_PLUGIN_ZIP}")
    message(STATUS "Embedding IDE plugin: ${CAJETA_PLUGIN_ZIP}")
else()
    # Zero-length stub matching the xxd symbol names. Written at configure time
    # (no command) so the source always exists; CONFIGURE_DEPENDS on the glob
    # re-runs cmake and flips to the xxd path once a zip appears.
    file(WRITE ${CAJETA_PLUGIN_EMBED}
        "// AUTO-GENERATED stub: no IDE plugin zip available at build time.\n"
        "unsigned char cajeta_plugin_zip[1] = { 0 };\n"
        "unsigned int cajeta_plugin_zip_len = 0;\n")
    message(STATUS "No IDE plugin zip under ide-plugins/idea/build/distributions/; embedding stub")
endif()
set_source_files_properties(${CAJETA_PLUGIN_EMBED} PROPERTIES GENERATED TRUE)
list(APPEND GENERATED_SOURCES ${CAJETA_PLUGIN_EMBED})
# ------------------------------------------------------------------------------

# Isolate the llvm::Error consumers (toString/cantFail/consumeError) used by
# the in-process JIT host in one TU and compile it -fno-rtti when the host LLVM
# has RTTI off (ROCm / many upstream builds) — same rationale as
# test/CMakeLists.txt's handling of JitErrorShim.cpp. No-op when LLVM has RTTI.
if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/cajeta/jit/CajetaJitErrorShim.cpp
            PROPERTIES COMPILE_OPTIONS "-fno-rtti")
endif()

# ------------------------------------------------------------------------------
# Embedded `cajeta init` archetype templates.
#
# samples/buildtool/<type>/ doubles as the on-disk worked example AND
# the source-of-truth for what `cajeta init <type>` writes. CMake walks
# those trees here and emits a generated .cpp that embeds the bytes.
# This eliminates drift between the samples and the archetypes the
# binary ships.
#
# Convention: only `cajeta.json` and anything under `src/` is embedded.
# `run.sh` and `README.md` describe the SAMPLE (where it lives in the
# repo, what today's CLI surface can exercise) — that's sample-meta,
# not what a freshly-init'd project needs. The init command writes a
# minimal next-steps message instead.
set(CAJETA_INIT_TEMPLATE_ROOT ${PROJECT_SOURCE_DIR}/samples/buildtool)
set(CAJETA_INIT_TEMPLATE_NAMES basic library workspace multi-binary melt)
set(CAJETA_INIT_TEMPLATE_FILES "")
set(CAJETA_INIT_MANIFEST_ROWS "")
foreach(tname ${CAJETA_INIT_TEMPLATE_NAMES})
    set(troot ${CAJETA_INIT_TEMPLATE_ROOT}/${tname})
    if (NOT EXISTS ${troot})
        message(FATAL_ERROR
            "init template '${tname}' missing: ${troot} does not exist")
    endif()
    # Manifest (always one). Glob picks up the cajeta.json at the root
    # and per-member manifests under workspace/.
    file(GLOB_RECURSE manifests CONFIGURE_DEPENDS ${troot}/cajeta.json
                                                  ${troot}/*/cajeta.json
                                                  ${troot}/*/*/cajeta.json
                                                  ${troot}/*/*/*/cajeta.json)
    # Source files (zero or more — `melt` has none).
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${troot}/src/*.cajeta
                                                ${troot}/*/src/*.cajeta
                                                ${troot}/*/*/src/*.cajeta
                                                ${troot}/*/*/*/src/*.cajeta
                                                ${troot}/*/*/*/*/src/*.cajeta)
    list(SORT manifests)
    list(SORT sources)
    foreach(abs ${manifests} ${sources})
        file(RELATIVE_PATH rel "${troot}" "${abs}")
        list(APPEND CAJETA_INIT_TEMPLATE_FILES ${abs})
        list(APPEND CAJETA_INIT_MANIFEST_ROWS "${tname}\t${rel}\t${abs}")
    endforeach()
endforeach()

set(CAJETA_INIT_EMBED    ${CMAKE_CURRENT_BINARY_DIR}/cajeta_init_embedded.cpp)
set(CAJETA_INIT_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/cajeta_init_manifest.txt)

# Write the manifest at configure time. CONFIGURE_DEPENDS on the globs
# above re-runs cmake (and thus rewrites this manifest) when samples
# are added or removed. The custom-command's DEPENDS picks up edits.
file(WRITE "${CAJETA_INIT_MANIFEST}" "")
foreach(row ${CAJETA_INIT_MANIFEST_ROWS})
    file(APPEND "${CAJETA_INIT_MANIFEST}" "${row}\n")
endforeach()

add_custom_command(
    OUTPUT ${CAJETA_INIT_EMBED}
    COMMAND ${CMAKE_COMMAND}
            -DMANIFEST=${CAJETA_INIT_MANIFEST}
            -DEMBED_OUT=${CAJETA_INIT_EMBED}
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedInitTemplates.cmake
    DEPENDS ${CAJETA_INIT_TEMPLATE_FILES}
            ${CAJETA_INIT_MANIFEST}
            ${PROJECT_SOURCE_DIR}/cmake/EmbedInitTemplates.cmake
    COMMENT "Embedding cajeta init archetype templates into ${CAJETA_INIT_EMBED}"
    VERBATIM
)

set_source_files_properties(${CAJETA_INIT_EMBED} PROPERTIES GENERATED TRUE)
list(APPEND GENERATED_SOURCES ${CAJETA_INIT_EMBED})
# ------------------------------------------------------------------------------

# --- AMD in-process device-JIT fix: hide the static LLVM symbols from the -----
# executables' dynamic symbol table (comgr collision). HIP lazily dlopen's
# libamd_comgr.so, which bundles its OWN, ABI-incompatible LLVM. Our binaries
# statically link the fork LLVM and (via -rdynamic, for the JIT's dlsym of the
# __cajeta_* runtime symbols + the crash-handler backtrace) export EVERYTHING —
# so the loader interposes comgr's llvm::opt::* onto our LLVM → SIGSEGV. Giving
# JUST the LLVM archives hidden visibility (`--exclude-libs`) stops the leak
# without un-exporting the runtime symbols the JIT needs: cajeta keeps using its
# fork LLVM internally (static linkage, not via .dynsym); comgr uses its own.
# Linux/ELF + static-component LLVM only (the Windows dylib path doesn't apply).
if(NOT WIN32 AND NOT CAJETA_USE_LLVM_DYLIB)
    set(_caj_llvm_excl "")
    # Glob the archives rather than walk LLVM_LIBS: llvm_map_components_to_libnames
    # returns only the NAMED components, but the link line also pulls in their
    # transitive deps (libLLVMipo.a, libLLVMTransformUtils.a, ...), and any archive
    # left off this list leaks its llvm::* symbols into .dynsym for comgr to bind to.
    file(GLOB _caj_llvm_archives "${LLVM_LIBRARY_DIR}/libLLVM*.a" "${LLVM_LIBRARY_DIR}/liblld*.a")
    foreach(_caj_a IN LISTS _caj_llvm_archives)
        get_filename_component(_caj_n "${_caj_a}" NAME)
        string(APPEND _caj_llvm_excl "${_caj_n}:")
    endforeach()
    foreach(_caj_l IN LISTS LLVM_LIBS)
        if(NOT _caj_llvm_excl MATCHES "(^|:)lib${_caj_l}\\.a:")
            string(APPEND _caj_llvm_excl "lib${_caj_l}.a:")
        endif()
    endforeach()
    set(CAJETA_LLVM_EXCLUDE_LIBS "${_caj_llvm_excl}" CACHE INTERNAL
        "LLVM static archives to hide from dynamic export (comgr interposition fix)")
    # --exclude-libs can't reach the vague-linkage LLVM template instantiations that
    # land in cajeta's OWN objects; this version script localizes them by mangling.
    set(CAJETA_LLVM_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/hide-llvm-symbols.map"
        CACHE INTERNAL "Version script localizing llvm::* (comgr interposition fix)")
endif()

add_library(${CMAKE_PROJECT_NAME}_lib STATIC ${SOURCES} ${GENERATED_SOURCES})
target_link_libraries(${CMAKE_PROJECT_NAME}_lib
        PUBLIC
        ${ANTLR4_RUNTIME_LIB}
        pthread
        # lld libs MUST precede the LLVM component libs: lldELF/lldCommon
        # reference llvm::opt (LLVMOption) and llvm::TarWriter (LLVMSupport),
        # and GNU ld (MinGW) resolves static archives in a single left-to-
        # right pass. LLVM never references lld, so this ordering satisfies
        # every reference in one pass. (Apple ld64 rescans, so order is
        # immaterial there; on Linux CAJETA_LLD_LIBS is usually empty.)
        ${CAJETA_LLD_LIBS}
        ${LLVM_LIBS}
        glog::glog
        zstd
        ZLIB::ZLIB
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl)
# The native runtime (cajeta_runtime.c, embedded in this lib) uses
# BCryptGenRandom for its RNG on Windows — link the bcrypt system lib.
# Only the test binary actually pulls the native runtime object; on
# cajeta.exe this is a harmless unused import.
if(WIN32)
    # bcrypt: __cajeta_alloc/random helpers. ws2_32: the cajeta.io.net native
    # runtime (cajeta_net_*.c, #included into cajeta_runtime.c) calls Winsock —
    # WSAStartup/socket/bind/getaddrinfo/select/etc. crypt32: the OS trust-store
    # shim in cajeta_tls.c (CertOpenSystemStore / CertEnumCertificatesInStore,
    # NET-5.3). PUBLIC so every consumer (cajeta.exe, cajeta_test,
    # cajeta_debug_test) inherits the import lib.
    target_link_libraries(${CMAKE_PROJECT_NAME}_lib PUBLIC bcrypt ws2_32 crypt32)
endif()
target_include_directories(${CMAKE_PROJECT_NAME}_lib PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/runtime/native   # cajeta_xpu_abi.h — the XPU FFI contract
        ${LLVM_INCLUDE_DIRS}
        ${ANTLR4_INCLUDE_DIR}
        ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
        ${ANTLR_CajetaGrammarParser_OUTPUT_DIR})
if(CAJETA_HAS_LLD)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_lib PUBLIC CAJETA_HAS_LLD=1)
    target_include_directories(${CMAKE_PROJECT_NAME}_lib PUBLIC ${CAJETA_LLD_INCLUDE_DIR})
endif()
# The fork LLVM's own bin dir — its `ld.lld` is version-matched to the bitcode
# this compiler emits, so `--lto=thin` must link through it (a system lld may be
# a different LLVM version and reject the bitcode summary). Empty if unknown;
# linkExecutable falls back to `-fuse-ld=lld` (PATH) with a warning.
if(LLVM_TOOLS_BINARY_DIR)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_lib
        PUBLIC CAJETA_LLVM_TOOLS_BIN="${LLVM_TOOLS_BINARY_DIR}")
endif()
if(CAJETA_HAS_SPV_RAY_QUERY)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_lib PUBLIC CAJETA_HAS_SPV_RAY_QUERY=1)
endif()
if(CAJETA_HAS_SPV_COOP_MATRIX)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_lib PUBLIC CAJETA_HAS_SPV_COOP_MATRIX=1)
endif()

# OptiX (NVIDIA RT-core acceleration structures) — header-only SDK; the engine is
# the driver's nvoptix.dll (loaded lazily by optixInit), so this is a COMPILE-TIME-
# only dependency, detected by header presence (mirroring the runtime's
# __has_include gating). Found -> src/cajeta/xpu/nvidia/OptixAccel.cpp compiles its
# real path + links cfgmgr32 (the OptiX DLL loader); absent -> that TU is stubs and
# the software-BVH ray-query floor is unaffected. Override with -DCAJETA_OPTIX_ROOT.
# Shares the CAJETA_OPTIX_INCLUDE_DIR / CAJETA_CUDA_INCLUDE_DIR cache vars with
# test/CMakeLists.txt (find_path is cached -> one detection).
find_path(CAJETA_OPTIX_INCLUDE_DIR optix.h
        HINTS "${CAJETA_OPTIX_ROOT}/include" "$ENV{OptiX_INSTALL_DIR}/include"
              "$ENV{OPTIX_ROOT}/include"
        PATHS "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.1.0/include"
              "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.0.0/include"
              "/opt/optix/include"
        DOC "OptiX SDK include directory (contains optix.h)")
find_path(CAJETA_CUDA_INCLUDE_DIR cuda.h
        HINTS "$ENV{CUDA_PATH}/include"
        PATHS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.9/include"
        DOC "CUDA toolkit include directory (cuda.h, required by the OptiX headers)")
if(CAJETA_OPTIX_INCLUDE_DIR AND CAJETA_CUDA_INCLUDE_DIR)
    message(STATUS "OptiX AS runtime ENABLED: ${CAJETA_OPTIX_INCLUDE_DIR}")
    target_compile_definitions(${CMAKE_PROJECT_NAME}_lib PUBLIC CAJETA_HAS_OPTIX=1)
    target_include_directories(${CMAKE_PROJECT_NAME}_lib PUBLIC
            ${CAJETA_OPTIX_INCLUDE_DIR} ${CAJETA_CUDA_INCLUDE_DIR})
    if(WIN32)
        target_link_libraries(${CMAKE_PROJECT_NAME}_lib PUBLIC cfgmgr32 advapi32)
    endif()
else()
    message(STATUS "OptiX AS runtime DISABLED (optix.h / cuda.h not found; "
            "software ray-query floor unaffected)")
endif()

# `cajeta doc` subcommand. DocCommand.cpp + the cajetadoc engine (the
# cajetadoc_core OBJECT library) are compiled/linked into the executable only,
# so cajeta_lib (the whole compiler) is untouched and not forced to recompile.
# cajetadoc_core's objects carry the antlr parser refs they need; the executable
# also links cajeta_lib, which defines those same parser symbols. When cajetadoc
# is disabled (-DCAJETA_BUILD_CAJETADOC=OFF) the stub branch of DocCommand.cpp
# compiles instead (no CAJETA_HAS_DOC, no cajetadoc include needed).
add_executable(${CMAKE_PROJECT_NAME} main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/cajeta/cli/DocCommand.cpp)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_PROJECT_NAME}_lib)
# Native-deps unit 12 (jit-run unblock): the in-process JIT resolves the
# standalone native runtime — the TLS natives (`__cajeta_tls_*`, kept OUT of the
# embedded JIT bitcode) and `cajeta_xpu_optix_*` — via dlsym on the driver's own
# symbols. main.cpp references none of them statically, so without this the
# driver drops the TLS object and never exports the optix symbols, and `jit-run`
# of any non-trivial program fails ("Symbols not found"). Mirror the cajeta_test
# target: force-link the TLS object + OpenSSL and export symbols (`-rdynamic`).
if(NOT WIN32)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
            ${CAJETA_TLS_OBJ} OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -rdynamic)
    # Don't export the static LLVM symbols (HIP's bundled LLVM would clash) —
    # the __cajeta_* runtime symbols the JIT dlsym's stay exported. `--exclude-libs`
    # is a GNU-ld/LLD option (Linux + mingw); Apple's ld64 rejects it, and macOS has
    # no HIP/ROCm to clash with, so skip it there.
    if(CAJETA_LLVM_EXCLUDE_LIBS AND NOT APPLE)
        target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
                "-Wl,--exclude-libs,${CAJETA_LLVM_EXCLUDE_LIBS}")
    endif()
    if(CAJETA_LLVM_VERSION_SCRIPT AND NOT APPLE)
        target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
                "-Wl,--version-script=${CAJETA_LLVM_VERSION_SCRIPT}")
        set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY
                LINK_DEPENDS "${CAJETA_LLVM_VERSION_SCRIPT}")
    endif()
endif()
if(TARGET cajetadoc_core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE cajetadoc_core)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAJETA_HAS_DOC=1)
endif()
