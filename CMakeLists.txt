CMAKE_MINIMUM_REQUIRED(VERSION 3.25 FATAL_ERROR)

# Canonical version source: the VERSION file at the repo root. Bumped
# manually as part of the release process (see RELEASING.md). Baked
# into the compiler binary so `cajeta --version` reports it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CAJETA_VERSION)
string(STRIP "${CAJETA_VERSION}" CAJETA_VERSION)

project(cajeta VERSION ${CAJETA_VERSION})

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
# file(GET_RUNTIME_DEPENDENCIES) (used by the Windows install DLL-bundling)
# normalizes paths before matching under CMP0207; opt in where available.
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
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
    # D9: the bundled IDE plugin version tracks the repo VERSION. Used by
    # `cajeta ide` for display; the release build also syncs the Gradle
    # pluginVersion to this same string.
    CAJETA_PLUGIN_VERSION="${CAJETA_VERSION}"
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

# cajetadoc — the documentation generator (plans/docs/cajetadoc-tool.md). Reuses the
# checked-in antlr-generated front end and links only the antlr4 runtime (no
# LLVM). Added BEFORE src/ so `cajetadoc_core` exists when src/ conditionally
# links it into cajeta_lib to expose the `cajeta doc` subcommand. Opt-out via
# -DCAJETA_BUILD_CAJETADOC=OFF.
option(CAJETA_BUILD_CAJETADOC "Build the cajetadoc documentation generator" ON)
if(CAJETA_BUILD_CAJETADOC)
    add_subdirectory(tools/cajetadoc)
endif()

add_subdirectory(src)
add_subdirectory(test)

# Debugger / in-process JIT host sub-project test target. Separate executable
# (cajeta_debug_test), kept out of the main `cajeta_test` battery and the
# cajeta_tests flow on purpose — its own TDD harness. See debug-tests/CMakeLists.txt.
add_subdirectory(debug-tests)

# =============================================================================
# Install rules + packaging (plan/installer-plan.md §3 — foundation)
#
# Layout follows the "single payload dir + symlink into PATH" principle (D5):
#   Unix:    <prefix>/lib/cajeta/cajeta        (the executable payload)
#            <prefix>/bin/cajeta -> ../lib/cajeta/cajeta   (PATH entry, symlink)
#   Windows: <prefix>/bin/cajeta.exe           (directly on PATH; no symlink)
# CPack then wraps this same install tree into .deb/.rpm/.msi/.pkg/.tar.gz.
# =============================================================================
include(GNUInstallDirs)

if(WIN32)
  # Windows: the binary lives directly under bin/ (e.g. C:\Program Files\Cajeta\bin).
  # D2 (Windows arm): the MinGW build links the monolithic libLLVM.dll + several
  # other mingw-w64 DLLs that aren't present on a stock Windows box, so we must
  # ship them beside cajeta.exe. Resolve the transitive closure at install time
  # via RUNTIME_DEPENDENCY_SET (rather than a hardcoded list that drifts with the
  # LLVM/toolchain version on each runner) and drop the resolved DLLs into bin/.
  get_filename_component(CAJETA_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  install(TARGETS cajeta
          RUNTIME_DEPENDENCY_SET cajeta_runtime_deps
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT cajeta)
  install(RUNTIME_DEPENDENCY_SET cajeta_runtime_deps
          # System DLLs we must NOT bundle: the API-set stubs and anything that
          # resolves out of the Windows system directories.
          PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
          POST_EXCLUDE_REGEXES ".*[/\\\\][Ss]ystem32[/\\\\].*" ".*[/\\\\]SysWOW64[/\\\\].*"
          DIRECTORIES "${CAJETA_MINGW_BIN}"
          DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT cajeta)
else()
  # Unix: payload under lib/cajeta, with a relative symlink in bin/ onto PATH.
  set(CAJETA_PAYLOAD_DIR "${CMAKE_INSTALL_LIBDIR}/cajeta")
  install(TARGETS cajeta
          RUNTIME DESTINATION "${CAJETA_PAYLOAD_DIR}"
          COMPONENT cajeta)
  # Relative link target (../lib/cajeta/cajeta) so it stays valid whether the
  # package is unpacked at the real root or a DESTDIR-staged prefix.
  file(RELATIVE_PATH CAJETA_SYMLINK_TARGET
       "${CMAKE_INSTALL_FULL_BINDIR}"
       "${CMAKE_INSTALL_FULL_LIBDIR}/cajeta/cajeta")
  install(CODE "
    set(_bindir \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}\")
    file(MAKE_DIRECTORY \"\${_bindir}\")
    execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
                    \"${CAJETA_SYMLINK_TARGET}\" \"\${_bindir}/cajeta\")
    message(STATUS \"cajeta: symlinked \${_bindir}/cajeta -> ${CAJETA_SYMLINK_TARGET}\")
  " COMPONENT cajeta)
endif()

# Documentation + license + version stamp.
install(FILES
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
          "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        COMPONENT cajeta)

# Man page (Unix only; Windows has no man infrastructure).
if(NOT WIN32)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/man/cajeta.1"
          DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
          COMPONENT cajeta)
endif()

# --- CPack core metadata (D6); per-generator knobs in cmake/CPackOptions.cmake.
set(CPACK_PACKAGE_NAME "cajeta")
set(CPACK_PACKAGE_VENDOR "Julian Klappenbach")
set(CPACK_PACKAGE_CONTACT "Julian Klappenbach <jklappenbach@gmail.com>")
set(CPACK_PACKAGE_VERSION "${CAJETA_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "The Cajeta language compiler (self-contained: embedded runtime + stdlib)")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jklappenbach/cajeta")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Cajeta")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
# The WiX generator rejects a license file without a .txt/.rtf extension, and
# our repo LICENSE is extension-less. Stage a .txt copy in the build tree and
# hand its path to CPackOptions.cmake (CPACK_-prefixed vars are exported into
# the per-generator config) so the WIX branch can point at it.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
               "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE_TXT "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
# cajeta-<ver>-<arch>; per-generator config can override (e.g. DEB wants amd64).
set(CPACK_PACKAGE_FILE_NAME "cajeta-${CAJETA_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
# Only ship the `cajeta` component (excludes any test/dev install artifacts).
set(CPACK_COMPONENTS_ALL cajeta)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackOptions.cmake")
include(CPack)
