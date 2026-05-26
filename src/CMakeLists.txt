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

# LLVM 18 on Ubuntu 24.04 expects an IMPORTED target zstd::libzstd_shared,
# but libzstd-dev (which ships the cmake config) is not always installed.
# Create a fallback IMPORTED target pointing at the system shared library.
if(NOT TARGET zstd::libzstd_shared)
    find_library(ZSTD_LIBRARY
            NAMES libzstd.so libzstd.so.1
            PATHS /usr/lib/x86_64-linux-gnu /usr/lib
            NO_DEFAULT_PATH)
    if(ZSTD_LIBRARY)
        add_library(zstd::libzstd_shared SHARED IMPORTED)
        set_target_properties(zstd::libzstd_shared PROPERTIES
                IMPORTED_LOCATION ${ZSTD_LIBRARY})
        message(STATUS "Created fallback zstd::libzstd_shared -> ${ZSTD_LIBRARY}")
    else()
        message(FATAL_ERROR "Could not locate libzstd. Install libzstd-dev or libzstd1.")
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
find_package(ANTLR REQUIRED)

# System antlr4 runtime headers (Debian/Ubuntu install at /usr/include/antlr4-runtime).
find_path(ANTLR4_INCLUDE_DIR
        NAMES antlr4-runtime.h
        PATH_SUFFIXES antlr4-runtime
        PATHS /usr/include /usr/local/include)
find_library(ANTLR4_RUNTIME_LIB
        NAMES antlr4-runtime
        PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib)
if(NOT ANTLR4_INCLUDE_DIR OR NOT ANTLR4_RUNTIME_LIB)
    message(FATAL_ERROR "Could not locate antlr4-runtime headers/library. Install libantlr4-runtime-dev.")
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

# Resolve LLVM component libraries. The compiler initializes all targets at
# startup, so we need every backend linked in.
llvm_map_components_to_libnames(LLVM_LIBS
        support core irreader option
        target mc passes analysis bitwriter bitreader
        orcjit executionengine runtimedyld linker
        AllTargetsAsmParsers
        AllTargetsCodeGens
        AllTargetsDescs
        AllTargetsInfos)

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
    set(CAJETA_LLD_LIBS ${CAJETA_LLD_ELF_LIB} ${CAJETA_LLD_COMMON_LIB})
    set(CAJETA_HAS_LLD ON)
    message(STATUS "lld libraries found; --emit=exe will use lld in-process.")
else()
    set(CAJETA_LLD_LIBS "")
    set(CAJETA_HAS_LLD OFF)
    message(STATUS "lld libraries NOT found; --emit=exe will print a hint. Install "
            "lld-${LLVM_VERSION_MAJOR}-dev (Debian/Ubuntu) to enable in-process linking.")
endif()

set(BINARY ${CMAKE_PROJECT_NAME})
file(GLOB_RECURSE SOURCES LIST_DIRECTORIES false *.h *.cpp)
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
find_program(CAJETA_CLANG NAMES "clang-${LLVM_VERSION_MAJOR}" clang
        PATHS /usr/lib/llvm-${LLVM_VERSION_MAJOR}/bin /usr/bin /usr/local/bin)
find_program(CAJETA_XXD NAMES xxd PATHS /usr/bin /usr/local/bin)
if(NOT CAJETA_CLANG)
    message(FATAL_ERROR "clang not found; needed to compile cajeta runtime to bitcode.")
endif()
if(NOT CAJETA_XXD)
    message(FATAL_ERROR "xxd not found; install vim-common (Debian/Ubuntu) or equivalent.")
endif()

set(CAJETA_RT_SRC ${PROJECT_SOURCE_DIR}/runtime/native/cajeta_runtime.c)
set(CAJETA_RT_BC  ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime.bc)
set(CAJETA_RT_EMBED ${CMAKE_CURRENT_BINARY_DIR}/cajeta_runtime_embedded.cpp)

add_custom_command(
        OUTPUT ${CAJETA_RT_BC}
        COMMAND ${CAJETA_CLANG} -c -emit-llvm -O2 -fno-builtin
                -ffunction-sections -fdata-sections
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
        ${CAJETA_STDLIB_ROOT}/cajeta/io/file
        ${CAJETA_STDLIB_ROOT}/cajeta/hash
        ${CAJETA_STDLIB_ROOT}/cajeta/collection
        ${CAJETA_STDLIB_ROOT}/cajeta/wire
        ${CAJETA_STDLIB_ROOT}/cajeta/codec/json)
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

add_library(${CMAKE_PROJECT_NAME}_lib STATIC ${SOURCES} ${GENERATED_SOURCES})
target_link_libraries(${CMAKE_PROJECT_NAME}_lib
        PUBLIC
        ${ANTLR4_RUNTIME_LIB}
        pthread
        ${LLVM_LIBS}
        ${CAJETA_LLD_LIBS}
        glog::glog)
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
