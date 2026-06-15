# Per-generator CPack configuration (CPACK_PROJECT_CONFIG_FILE).
#
# CPack loads this file once per generator at `cpack` time, with
# CPACK_GENERATOR set to the generator currently being produced. Branching
# on it here keeps format-specific knobs out of the main CMake configure
# (where they would all be set unconditionally regardless of what's built).
#
# See plan/installer-plan.md: §3 (foundation), §4 (deb/rpm), §5 (WiX),
# §6 (productbuild). Common metadata (vendor, contact, version, license,
# homepage) is set at configure time in the top-level CMakeLists.txt; this
# file only carries the per-generator specializations.

# --- Debian (.deb) --- plan §4
if(CPACK_GENERATOR MATCHES "DEB")
  # Idiomatic dpkg name: cajeta_<ver>_<arch>.deb (arch auto = amd64 / arm64).
  set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Julian Klappenbach <jklappenbach@gmail.com>")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/jklappenbach/cajeta")
  set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
  # Self-contained binary (D2): let shlibdeps derive only the base-system
  # runtime deps (libc6 / libstdc++6 / libgcc-s1) from the actual ELF, rather
  # than declaring a hand-written (and inevitably wrong) dependency list.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # arch label (amd64 / arm64) is auto-derived by dpkg from the build host.
  # postinst hint pointing at `cajeta ide install` (D8). CPack normalizes the
  # script's mode to 0755 in the control archive.
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${CMAKE_CURRENT_LIST_DIR}/../packaging/linux/deb-postinst")
endif()

# --- RPM (.rpm) --- plan §4
if(CPACK_GENERATOR MATCHES "RPM")
  # Idiomatic rpm name: cajeta-<ver>-<release>.<arch>.rpm (arch = x86_64 / aarch64).
  set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
  set(CPACK_RPM_PACKAGE_URL "https://github.com/jklappenbach/cajeta")
  # Auto-compute Requires: from the linked shared objects (base-system only,
  # per D2). AUTOREQ on / AUTOPROV off keeps us from advertising our private
  # payload .so names as provides.
  set(CPACK_RPM_PACKAGE_AUTOREQ ON)
  set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
  # %post hint pointing at `cajeta ide install` (D8).
  set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
      "${CMAKE_CURRENT_LIST_DIR}/../packaging/linux/rpm-post.sh")
  # Don't let rpmbuild claim ownership of standard dirs like /usr/lib,
  # /usr/bin, /usr/share/man — they belong to filesystem/glibc.
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      "/usr/lib" "/usr/bin" "/usr/share" "/usr/share/man" "/usr/share/man/man1"
      "/usr/share/doc")
endif()

# --- Windows (WiX MSI) --- plan §5
if(CPACK_GENERATOR MATCHES "WIX")
  # WiX Toolset v4+ ships a single `wix` CLI (the dotnet global tool) rather
  # than v3's candle/light pair. CPackWIX only selects that codepath when
  # CPACK_WIX_VERSION >= 4, so declare it (auto-detected from the installed
  # tool if left unset on newer CMake, but we pin to be explicit / CI-stable).
  if(NOT CPACK_WIX_VERSION)
    set(CPACK_WIX_VERSION 4)
  endif()
  # Stable upgrade GUID so a newer MSI upgrades rather than installs
  # side-by-side. Generated once; must never change across releases.
  set(CPACK_WIX_UPGRADE_GUID "8F3C5A2E-1B47-4D6A-9E0C-2A7B6F4D3C81")
  set(CPACK_WIX_PROGRAM_MENU_FOLDER "Cajeta")
  set(CPACK_WIX_INSTALL_SCOPE "perMachine")
  # WiX needs a .txt/.rtf license; the top-level CMake staged one for us.
  if(CPACK_RESOURCE_FILE_LICENSE_TXT)
    set(CPACK_RESOURCE_FILE_LICENSE "${CPACK_RESOURCE_FILE_LICENSE_TXT}")
  endif()
  # Put the install dir's bin/ on the system PATH (plan §5). The patch file
  # injects a WiX <Environment> element into the cajeta.exe component.
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/wix/path-patch.xml")
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_LIST_DIR}/wix/path-patch.xml")
  endif()
endif()

# --- macOS (.pkg / productbuild) --- plan §6
if(CPACK_GENERATOR MATCHES "productbuild|PackageMaker")
  set(CPACK_PRODUCTBUILD_IDENTIFIER "dev.cajeta.compiler")
  # productbuild only accepts .rtf/.rtfd/.html/.txt for the License/ReadMe panes
  # (it errors "Bad file extension specified" otherwise). Our repo LICENSE is
  # extension-less and README is .md, so point both at the .txt copies the
  # top-level CMake staged — same fix the WIX branch uses for the license.
  if(CPACK_RESOURCE_FILE_LICENSE_TXT)
    set(CPACK_RESOURCE_FILE_LICENSE "${CPACK_RESOURCE_FILE_LICENSE_TXT}")
  endif()
  if(CPACK_RESOURCE_FILE_README_TXT)
    set(CPACK_RESOURCE_FILE_README "${CPACK_RESOURCE_FILE_README_TXT}")
  endif()
  # Install the payload under /usr/local (not "/"): /usr/local/lib/cajeta/cajeta
  # + /usr/local/bin/cajeta symlink (§2 macOS path row). Signing + notarization
  # (Developer ID Installer → notarytool → stapler) remain a Mac-runner task.
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
  # postinstall: best-effort `cajeta ide install` as the console user (D8).
  # CPACK_POSTFLIGHT_<COMPONENT>_SCRIPT — component is "cajeta" (upper-cased).
  set(CPACK_POSTFLIGHT_CAJETA_SCRIPT
      "${CMAKE_CURRENT_LIST_DIR}/../packaging/macos/postinstall")
endif()
