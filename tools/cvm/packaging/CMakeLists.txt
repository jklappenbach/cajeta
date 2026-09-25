# Native packages for cvm.
#
# STANDALONE on purpose. cvm is built by the cajeta build tool from
# tools/cvm/cajeta.json, so it is not a target in the compiler's CMake project
# and no CPack generator there can see it. This project exists only to hand an
# already-built binary to CPack, which is why it compiles nothing: the package
# must carry the same bytes the bare release asset carries, not a second build.
#
# Configure with the binary's path and the version to declare, then run the
# generator the platform wants:
#
#   cmake -S tools/cvm/packaging -B build/cvm-pkg \
#         -DCVM_BINARY=tools/cvm/build/cvm -DCVM_PACKAGE_VERSION=0.29.1
#   cmake --build build/cvm-pkg --target package   # or: cd build/cvm-pkg && cpack -G DEB
#
# The package installs ONE executable and nothing else. cvm needs no libraries,
# no headers and no data: it downloads a toolchain into $CAJETA_HOME.
cmake_minimum_required(VERSION 3.22)

# NONE: there is no compiler to find, and asking for one makes this project
# fail to configure on a host that has a cvm binary but no C++ toolchain.
project(cajeta-cvm LANGUAGES NONE)

set(CVM_BINARY "" CACHE FILEPATH "Path to the already-built cvm executable")
# The version a package manager will order releases by. cvm's own declared
# version in cajeta.json has been 0.1.0 across every release, so it cannot
# serve here: two releases would both be 0.1.0 and no upgrade would ever be
# offered. See specs/cvm-installer-spec.md 3.2 and open question 6.1.
set(CVM_PACKAGE_VERSION "" CACHE STRING "Version to declare for the package")

if(NOT CVM_BINARY OR NOT EXISTS "${CVM_BINARY}")
    message(FATAL_ERROR
            "CVM_BINARY must point at a built cvm executable (got '${CVM_BINARY}'). "
            "Build it with `cajeta release` in tools/cvm.")
endif()
if(NOT CVM_PACKAGE_VERSION)
    message(FATAL_ERROR "CVM_PACKAGE_VERSION must be set.")
endif()

# PROGRAMS rather than FILES: it installs mode 0755. That is the entire point
# of this channel, since the bare asset arrives at 664 and a reader has to be
# told to chmod it.
# The COMPONENT is named rather than left Unspecified because CPack derives the
# WiX component id from it plus the install path, and wix/path-patch.xml has to
# name that id to attach the PATH entry to it.
install(PROGRAMS "${CVM_BINARY}"
        DESTINATION bin
        RENAME "cvm${CMAKE_EXECUTABLE_SUFFIX}"
        COMPONENT cvm)
set(CPACK_COMPONENTS_ALL cvm)

# ---- CPack --------------------------------------------------------------
# Name distinct from the compiler's `cajeta`, so a package manager can hold
# both and neither owns the other's files.
set(CPACK_PACKAGE_NAME "cajeta-cvm")
set(CPACK_PACKAGE_VERSION "${CVM_PACKAGE_VERSION}")
set(CPACK_PACKAGE_VENDOR "Julian Klappenbach")
set(CPACK_PACKAGE_CONTACT "Julian Klappenbach <jklappenbach@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jklappenbach/cajeta")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Cajeta Version Manager — installs and switches Cajeta toolchains")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Cajeta")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES OFF)   # already built as the release wants it

# /usr, so bin lands on the default PATH. The default CPack prefix for an
# archive-ish generator is not on PATH, and an installer whose payload is not
# reachable has not installed anything as far as the reader is concerned.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_GENERATOR "DEB;RPM")
elseif(APPLE)
    set(CPACK_GENERATOR "productbuild")
    # /usr is not writable on a sealed system volume, and /usr/local/bin is on
    # PATH on macOS.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
elseif(WIN32)
    set(CPACK_GENERATOR "WIX")
endif()

# ---- WiX -----------------------------------------------------------------
# Mirrors cmake/CPackOptions.cmake's WIX branch. The upgrade GUID is its OWN,
# generated once for this package: sharing the compiler's would make each
# installer try to upgrade the other out of existence.
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_UPGRADE_GUID "06783F91-9B76-4370-8F23-E3229F3E672F")
set(CPACK_WIX_INSTALL_SCOPE "perMachine")
# Program Files is not on PATH, so the patch puts bin/ there. Without it the
# MSI installs a cvm the shell cannot find.
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/wix/path-patch.xml")
# WiX and productbuild both refuse a license without a .txt/.rtf extension, and
# the repo's LICENSE has none. Stage a copy that does.
set(CVM_LICENSE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSE")
if(EXISTS "${CVM_LICENSE_SRC}")
    configure_file("${CVM_LICENSE_SRC}" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
endif()

set(CPACK_PRODUCTBUILD_IDENTIFIER "dev.cajeta.cvm")

# Idiomatic per-format names: cajeta-cvm_<ver>_<arch>.deb and
# cajeta-cvm-<ver>-<arch>.rpm, which is what a reader expects to see.
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Julian Klappenbach <jklappenbach@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
# Let shlibdeps read the actual ELF rather than declaring a hand-written
# dependency list that will be wrong the first time the link changes.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
# /usr/bin is owned by filesystem; claiming it makes rpm refuse to install
# alongside anything else.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION "/usr/bin")

include(CPack)
