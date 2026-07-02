CMAKE_MINIMUM_REQUIRED(VERSION 3.25 FATAL_ERROR)

# Canonical version source: the VERSION file at the repo root. Bumped
# manually as part of the release process (see RELEASING.md). Baked
# into the compiler binary so `cajeta --version` reports it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CAJETA_VERSION)
string(STRIP "${CAJETA_VERSION}" CAJETA_VERSION)

# CAJETA_VERSION is the full, human-facing version string and may carry a
# semver pre-release suffix (e.g. "0.7.0-rc1") — that's what gets baked into
# `cajeta --version` and used for asset/package naming. CMake's project(VERSION)
# however only accepts a numeric dotted form (<major>[.<minor>[.<patch>]]); a
# "-rc1" suffix makes configure fail outright. Strip the suffix for project()
# only, so pre-release tags (a documented release path) configure cleanly while
# the binary still reports the true pre-release version.
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" CAJETA_VERSION_NUMERIC "${CAJETA_VERSION}")
if(NOT CAJETA_VERSION_NUMERIC)
    message(FATAL_ERROR "VERSION file '${CAJETA_VERSION}' has no numeric version prefix")
endif()

project(cajeta VERSION ${CAJETA_VERSION_NUMERIC})

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
# file(GET_RUNTIME_DEPENDENCIES) (used by the Windows install DLL-bundling)
# normalizes paths before matching under CMP0207; opt in where available.
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")

# Build acceleration: route compiles through ccache when present so unchanged
# translation units are served from cache on rebuilds/branch-switches. Opt-out
# with -DCAJETA_USE_CCACHE=OFF.
option(CAJETA_USE_CCACHE "Use ccache as the compiler launcher when available" ON)
if(CAJETA_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
    endif()
endif()

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

# libcajeta_amdtex — optional AMD HIP mipmap/cube texture-emulation helper
# (vendored addrlib + C-ABI wrapper, dlopen'd by the runtime). Built before src/
# so it's available when the test/AOT paths exercise the AMD texture backend.
add_subdirectory(runtime/native/amd)

add_subdirectory(src)

# cajetadoc_core (configured above, before src/) #includes the ANTLR-generated
# CajetaParser.h but doesn't compile the generated sources, so it has no
# build-order edge to the generation. Add it here, now that both targets exist,
# so a parallel build can't race Ingest.cpp ahead of the generated header.
if(TARGET cajetadoc_core AND TARGET cajeta_antlr_generated)
    add_dependencies(cajetadoc_core cajeta_antlr_generated)
endif()

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
  # When cajeta links the monolithic LLVM dylib (the slim cajeta-llvm Windows
  # distribution), libLLVM-<ver>.dll lives in the LLVM toolchain's bin, NOT the
  # mingw bin — add it to the resolver's search dirs so it gets bundled beside
  # cajeta.exe. LLVM_DIR (cached by find_package(LLVM) in src/) is
  # <llvm>/lib/cmake/llvm; its bin is three dirs up + /bin. Harmless when the
  # static toolchain is used (no libLLVM.dll dependency to resolve).
  set(CAJETA_LLVM_BIN "")
  if(LLVM_DIR)
    get_filename_component(CAJETA_LLVM_BIN "${LLVM_DIR}/../../../bin" ABSOLUTE)
  endif()
  install(TARGETS cajeta
          RUNTIME_DEPENDENCY_SET cajeta_runtime_deps
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT cajeta)
  install(RUNTIME_DEPENDENCY_SET cajeta_runtime_deps
          # System DLLs we must NOT bundle: the API-set stubs and anything that
          # resolves out of the Windows system directories.
          PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
          POST_EXCLUDE_REGEXES ".*[/\\\\][Ss]ystem32[/\\\\].*" ".*[/\\\\]SysWOW64[/\\\\].*"
          DIRECTORIES "${CAJETA_MINGW_BIN}" "${CAJETA_LLVM_BIN}"
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
  # Stage the symlink in the build tree and install it via a RELATIVE destination
  # so CPack relocates its location into the packaging prefix (.deb/.rpm -> /usr)
  # just like the payload. A baked absolute install(CODE) path is NOT relocated,
  # so it lands in the configure-time prefix (default /usr/local) and dangles.
  set(CAJETA_SYMLINK_STAGE "${CMAKE_CURRENT_BINARY_DIR}/cajeta-bin-symlink/cajeta")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cajeta-bin-symlink")
  file(CREATE_LINK "${CAJETA_SYMLINK_TARGET}" "${CAJETA_SYMLINK_STAGE}" SYMBOLIC)
  install(FILES "${CAJETA_SYMLINK_STAGE}"
          DESTINATION "${CMAKE_INSTALL_BINDIR}"
          COMPONENT cajeta)
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
# The WiX (Windows) and productbuild (macOS) generators both reject resource
# files without an accepted extension — WiX wants .txt/.rtf; productbuild wants
# .rtf/.rtfd/.html/.txt. Our repo LICENSE is extension-less and README is .md,
# so both are rejected as-is. Stage .txt copies in the build tree and hand their
# paths to CPackOptions.cmake (CPACK_-prefixed vars are exported into the
# per-generator config) so the WIX and productbuild branches can point at them.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
               "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE_TXT "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/README.md"
               "${CMAKE_CURRENT_BINARY_DIR}/README.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_README_TXT "${CMAKE_CURRENT_BINARY_DIR}/README.txt")
# cajeta-<ver>-<arch>; per-generator config can override (e.g. DEB wants amd64).
set(CPACK_PACKAGE_FILE_NAME "cajeta-${CAJETA_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
# Only ship the `cajeta` component (excludes any test/dev install artifacts).
set(CPACK_COMPONENTS_ALL cajeta)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackOptions.cmake")
include(CPack)
