CMAKE_MINIMUM_REQUIRED(VERSION 3.25 FATAL_ERROR)

# Canonical version source: the VERSION file at the repo root. Bumped
# manually as part of the release process (see RELEASING.md). Baked
# into the compiler binary so `cajeta --version` reports it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CAJETA_VERSION)
string(STRIP "${CAJETA_VERSION}" CAJETA_VERSION)

project(cajeta VERSION ${CAJETA_VERSION})

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")

# Windows/MSYS2 convenience: glog, LLVM, zstd, antlr4, etc. are mingw-w64
# packages whose *Config.cmake files live under C:/msys64/mingw64. Without
# that prefix on CMAKE_PREFIX_PATH, a bare `cmake` from a shell that doesn't
# have mingw64 on PATH fails at the first find_package (e.g.
# "Could not find a package configuration file provided by glog"). Seed the
# prefix here (only when the dir exists, so non-MSYS2 Windows setups and
# Linux/macOS are unaffected) so configure works regardless of which shell
# invoked it. An explicit -DCMAKE_PREFIX_PATH or a different MSYS root still
# wins — we append rather than overwrite.
if(WIN32 AND EXISTS "C:/msys64/mingw64/lib/cmake")
    list(APPEND CMAKE_PREFIX_PATH "C:/msys64/mingw64")
endif()

# Capture the short commit hash for `cajeta --version`. When git isn't
# available or the tree isn't a checkout (release tarball builds), fall
# back to "unknown".
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CAJETA_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RC)
    if(NOT GIT_RC EQUAL 0)
        set(CAJETA_GIT_HASH "unknown")
    endif()
else()
    set(CAJETA_GIT_HASH "unknown")
endif()

add_compile_definitions(
    CAJETA_VERSION="${CAJETA_VERSION}"
    CAJETA_GIT_HASH="${CAJETA_GIT_HASH}"
)

# macOS link-search-path fix. brew's lib directory (/opt/homebrew/lib on
# Apple Silicon, /usr/local/lib on Intel) isn't on ld's default library
# search path. LLVM's transitive deps emit bare `-lzstd` (and a few
# others); without -L<brew>/lib, ld fails with "library 'zstd' not found"
# even though the IMPORTED target zstd::libzstd_shared in src/CMakeLists.txt
# passed the full .dylib path. Set link_directories at TOP level (not in
# src/CMakeLists.txt) so the path propagates to every subdirectory's
# targets — including test/.
if(APPLE)
    execute_process(COMMAND brew --prefix
            OUTPUT_VARIABLE CAJETA_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE CAJETA_BREW_RC)
    if(CAJETA_BREW_RC EQUAL 0 AND CAJETA_BREW_PREFIX)
        message(STATUS "Adding brew lib dir to linker search path: ${CAJETA_BREW_PREFIX}/lib")
        link_directories(${CAJETA_BREW_PREFIX}/lib)
    endif()
endif()

include_directories(src)

# Enable CTest at the top level so gtest_discover_tests() in test/ actually
# registers its cases. Without this, add_test() in any subdirectory is a
# no-op: the generated CTestTestfile.cmake stays empty and `ctest -R ...`
# reports "No tests were found" even though cajeta_test.exe runs fine
# directly. Must precede add_subdirectory(test).
enable_testing()

add_subdirectory(src)
add_subdirectory(test)
