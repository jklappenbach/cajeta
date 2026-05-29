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
# libcrypto from OpenSSL backs `cajeta archive sign` / `verify-sig`
# (ed25519 detached signatures + PEM key parsing). See
# cajeta-docs/ArchiveManagement.md §8.
find_package(OpenSSL REQUIRED)

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
            orcjit executionengine runtimedyld linker
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
find_program(CAJETA_CLANG NAMES "clang-${LLVM_VERSION_MAJOR}" clang
        HINTS ${CAJETA_MSYS_BIN_HINTS}
        PATHS /usr/lib/llvm-${LLVM_VERSION_MAJOR}/bin /usr/bin /usr/local/bin)
find_program(CAJETA_XXD NAMES xxd
        HINTS ${CAJETA_MSYS_BIN_HINTS}
        PATHS /usr/bin /usr/local/bin)
if(NOT CAJETA_CLANG)
    message(FATAL_ERROR "clang not found; needed to compile cajeta runtime to bitcode.")
endif()
if(NOT CAJETA_XXD)
    message(FATAL_ERROR "xxd not found; install vim-common (Debian/Ubuntu) or equivalent.")
endif()

set(CAJETA_RT_SRC ${PROJECT_SOURCE_DIR}/runtime/native/cajeta_runtime.c)
set(CAJETA_RT_BC  ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime.bc)
set(CAJETA_RT_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime_embedded.cpp)

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
if(APPLE)
    list(APPEND CAJETA_RT_FLAGS "-fno-emulated-tls")
endif()

add_custom_command(
        OUTPUT ${CAJETA_RT_BC}
        COMMAND ${CAJETA_CLANG} -c -emit-llvm -O2 -fno-builtin
                -ffunction-sections -fdata-sections
                -I${CAJETA_XXHASH_INCLUDE_DIR}
                ${CAJETA_RT_FLAGS}
                -o ${CAJETA_RT_BC} ${CAJETA_RT_SRC}
        DEPENDS ${CAJETA_RT_SRC}
        COMMENT "Compiling Cajeta runtime to LLVM bitcode")

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

# Embed the cajeta stdlib `.cajeta` source files into the compiler binary.
# The Compiler iterates the resulting cajeta::stdlib::g_files manifest at
# parse time, treating each entry as a stdlib prelude source. Sources live
# under runtime/src/cajeta/ — see runtime/src/cajeta/error/ for the v1 set.
# Keeping them as real files (rather than inline C strings) makes the
# stdlib reviewable / editable like any other cajeta code.
set(CAJETA_STDLIB_ROOT ${PROJECT_SOURCE_DIR}/runtime/src)
# Explicit subset list rather than a recursive glob: the runtime/src tree
# also contains skeletal stubs (codec/, lang/string/, math/) that aren't
# parseable yet — embedding those would crash the compiler at first use.
# Add directories here as their `.cajeta` files become real and pass the
# stdlib parse pass. Glob inside the explicit subdirs so adding new
# error/* files (etc.) is automatic.
#
# Order within each directory doesn't matter: cross-file forward
# references for both "type-as-field" and "extends" use the placeholder
# mechanism, and CajetaModule::buildPendingPrototypes runs a fixed-point
# layout pass after parse so subclasses get their full inherited-field
# layout regardless of parse order.
set(CAJETA_STDLIB_DIRS
        ${CAJETA_STDLIB_ROOT}/cajeta/lang
        ${CAJETA_STDLIB_ROOT}/cajeta/lang/stream
        ${CAJETA_STDLIB_ROOT}/cajeta/error
        # Threading sync primitives (Lock/LockGuard today; Mutex/RwLock/
        # ConditionVariable/Channel/Semaphore land in later R7 sub-phases).
        # After cajeta/lang (needs Object) and cajeta/error (future
        # Channel close/timeout throws).
        ${CAJETA_STDLIB_ROOT}/cajeta/threading
        ${CAJETA_STDLIB_ROOT}/cajeta/io/file
        ${CAJETA_STDLIB_ROOT}/cajeta/hash
        ${CAJETA_STDLIB_ROOT}/cajeta/collection
        ${CAJETA_STDLIB_ROOT}/cajeta/wire
        ${CAJETA_STDLIB_ROOT}/cajeta/codec/json
        # Larger-than-memory collections. Listed after collection/wire/
        # io.file so its dependencies (BPlusTree shape, Encoder, File) are
        # already in the prelude when these parse.
        ${CAJETA_STDLIB_ROOT}/cajeta/collection/ltm
        ${CAJETA_STDLIB_ROOT}/cajeta/xpu/core)
set(CAJETA_STDLIB_FILES "")
foreach(dir ${CAJETA_STDLIB_DIRS})
    file(GLOB dir_files CONFIGURE_DEPENDS ${dir}/*.cajeta)
    # Sort within each directory only — the directory order in
    # CAJETA_STDLIB_DIRS is load-bearing: cajeta/lang (Object) must
    # parse before classes that auto-extend it. Without ordering,
    # alphabetical sort would put cajeta/error/* ahead of
    # cajeta/lang/Object and force every Throwable-family class
    # through the placeholder-resolution fallback unnecessarily.
    list(SORT dir_files)
    list(APPEND CAJETA_STDLIB_FILES ${dir_files})
endforeach()
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

# Isolate the llvm::Error consumers (toString/cantFail/consumeError) used by
# the in-process JIT host in one TU and compile it -fno-rtti when the host LLVM
# has RTTI off (ROCm / many upstream builds) — same rationale as
# test/CMakeLists.txt's handling of JitErrorShim.cpp. No-op when LLVM has RTTI.
if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/cajeta/jit/CajetaJitErrorShim.cpp
            PROPERTIES COMPILE_OPTIONS "-fno-rtti")
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
        OpenSSL::Crypto)
# The native runtime (cajeta_runtime.c, embedded in this lib) uses
# BCryptGenRandom for its RNG on Windows — link the bcrypt system lib.
# Only the test binary actually pulls the native runtime object; on
# cajeta.exe this is a harmless unused import.
if(WIN32)
    target_link_libraries(${CMAKE_PROJECT_NAME}_lib PUBLIC bcrypt)
endif()
target_include_directories(${CMAKE_PROJECT_NAME}_lib PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${LLVM_INCLUDE_DIRS}
        ${ANTLR4_INCLUDE_DIR}
        ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
        ${ANTLR_CajetaGrammarParser_OUTPUT_DIR})
if(CAJETA_HAS_LLD)
    target_compile_definitions(${CMAKE_PROJECT_NAME}_lib PUBLIC CAJETA_HAS_LLD=1)
    target_include_directories(${CMAKE_PROJECT_NAME}_lib PUBLIC ${CAJETA_LLD_INCLUDE_DIR})
endif()

add_executable(${CMAKE_PROJECT_NAME} main.cpp)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_PROJECT_NAME}_lib)
